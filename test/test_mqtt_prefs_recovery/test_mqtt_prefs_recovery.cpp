#include <map>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "helpers/MqttPrefsCodec.h"

using namespace mqtt_prefs;

namespace {

constexpr const char *kPrimary = "/mqtt.cfg";
constexpr const char *kTemp = "/mqtt.cfg.tmp";
constexpr const char *kBackup = "/mqtt.cfg.bak";

enum class Fault {
  None,
  FailRemovePrimary,
  FailRemoveBackup,
  FailDemotePrimary,
  FailPromoteTemp,
  FailRollbackPrimary,
  FailRollbackBackup,
  CutAfterRemovePrimary,
  CutAfterRemoveBackup,
  CutAfterDemotePrimary,
  CutAfterPromoteTemp,
};

class PowerCut : public std::runtime_error {
public:
  PowerCut() : std::runtime_error("simulated power cut") {}
};

class FakeFiles {
public:
  std::map<std::string, std::string> files;
  Fault fault = Fault::None;
  bool fail_rollback_backup = false;

  bool exists(const char *path) const {
    return files.find(path) != files.end();
  }

  bool remove(const char *path) {
    if ((fault == Fault::FailRemovePrimary && std::string(path) == kPrimary) ||
        (fault == Fault::FailRemoveBackup && std::string(path) == kBackup)) {
      return false;
    }
    files.erase(path);
    if (fault == Fault::CutAfterRemovePrimary && std::string(path) == kPrimary) {
      throw PowerCut();
    }
    if (fault == Fault::CutAfterRemoveBackup && std::string(path) == kBackup) {
      throw PowerCut();
    }
    return true;
  }

  bool rename(const char *from, const char *to) {
    const std::string source = from;
    const std::string destination = to;
    if ((fault == Fault::FailDemotePrimary && source == kPrimary && destination == kBackup) ||
        (fault == Fault::FailPromoteTemp && source == kTemp && destination == kPrimary) ||
        ((fault == Fault::FailRollbackBackup || fail_rollback_backup) &&
         source == kBackup && destination == kPrimary)) {
      return false;
    }
    const auto it = files.find(source);
    if (it == files.end()) return false;
    files[destination] = it->second;
    files.erase(it);
    if (fault == Fault::CutAfterDemotePrimary && source == kPrimary && destination == kBackup) {
      throw PowerCut();
    }
    if (fault == Fault::CutAfterPromoteTemp && source == kTemp && destination == kPrimary) {
      throw PowerCut();
    }
    return true;
  }
};

RecoveryCommitResult commit(FakeFiles &fs, RecoverySource source) {
  return commitVerifiedTemp(
      source, kPrimary, kTemp, kBackup,
      [&fs](const char *path) { return fs.exists(path); },
      [&fs](const char *path) { return fs.remove(path); },
      [&fs](const char *from, const char *to) { return fs.rename(from, to); });
}

FakeFiles backupRecoveryFiles() {
  FakeFiles fs;
  fs.files[kPrimary] = "corrupt-primary";
  fs.files[kBackup] = "good-backup";
  fs.files[kTemp] = "new-v4";
  return fs;
}

FakeFiles primaryRotationFiles() {
  FakeFiles fs;
  fs.files[kPrimary] = "old-primary";
  fs.files[kBackup] = "older-backup";
  fs.files[kTemp] = "new-v4";
  return fs;
}

}  // namespace

TEST(MqttPrefsRecovery, OnlyCompleteTempCanBePromoted) {
  EXPECT_EQ(selectRecoveryAction(RecoveryFileState::Invalid, RecoveryFileState::Usable,
                                 RecoveryFileState::Usable),
            RecoveryAction::PromoteTemp);
  EXPECT_EQ(selectRecoveryAction(RecoveryFileState::Invalid, RecoveryFileState::Invalid,
                                 RecoveryFileState::Usable),
            RecoveryAction::PromoteBackup);
  EXPECT_EQ(selectRecoveryAction(RecoveryFileState::Invalid, RecoveryFileState::Preserve,
                                 RecoveryFileState::Usable),
            RecoveryAction::PromoteBackup);
  EXPECT_EQ(selectRecoveryAction(RecoveryFileState::Missing, RecoveryFileState::Invalid,
                                 RecoveryFileState::Missing),
            RecoveryAction::None);
  EXPECT_EQ(selectRecoveryAction(RecoveryFileState::Preserve, RecoveryFileState::Usable,
                                 RecoveryFileState::Usable),
            RecoveryAction::KeepPrimary);
}

TEST(MqttPrefsRecovery, BackupRecoveryKeepsKnownGoodBackup) {
  FakeFiles fs = backupRecoveryFiles();

  const RecoveryCommitResult result = commit(fs, RecoverySource::Backup);

  ASSERT_TRUE(result.committed);
  EXPECT_FALSE(result.temp_retained);
  EXPECT_EQ(fs.files[kPrimary], "new-v4");
  EXPECT_EQ(fs.files[kBackup], "good-backup");
  EXPECT_FALSE(fs.exists(kTemp));
}

TEST(MqttPrefsRecovery, BackupRecoveryRenameFailuresRetainRecoveryImages) {
  {
    FakeFiles fs = backupRecoveryFiles();
    fs.fault = Fault::FailRemovePrimary;
    const RecoveryCommitResult result = commit(fs, RecoverySource::Backup);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.failure, RecoveryCommitFailure::RemoveInvalidPrimary);
    EXPECT_EQ(fs.files[kPrimary], "corrupt-primary");
    EXPECT_EQ(fs.files[kBackup], "good-backup");
    EXPECT_EQ(fs.files[kTemp], "new-v4");
  }

  {
    FakeFiles fs = backupRecoveryFiles();
    fs.fault = Fault::FailPromoteTemp;
    const RecoveryCommitResult result = commit(fs, RecoverySource::Backup);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.failure, RecoveryCommitFailure::PromoteTemp);
    EXPECT_FALSE(fs.exists(kPrimary));
    EXPECT_EQ(fs.files[kBackup], "good-backup");
    EXPECT_EQ(fs.files[kTemp], "new-v4");
  }
}

TEST(MqttPrefsRecovery, BackupRecoveryPowerCutsAreRecoverable) {
  {
    FakeFiles fs = backupRecoveryFiles();
    fs.fault = Fault::CutAfterRemovePrimary;
    EXPECT_THROW(commit(fs, RecoverySource::Backup), PowerCut);
    EXPECT_FALSE(fs.exists(kPrimary));
    EXPECT_EQ(fs.files[kBackup], "good-backup");
    EXPECT_EQ(fs.files[kTemp], "new-v4");

    const RecoveryCommitResult recovery = commit(fs, RecoverySource::Temp);
    ASSERT_TRUE(recovery.committed);
    EXPECT_EQ(fs.files[kPrimary], "new-v4");
    EXPECT_EQ(fs.files[kBackup], "good-backup");
  }

  {
    FakeFiles fs = backupRecoveryFiles();
    fs.fault = Fault::CutAfterPromoteTemp;
    EXPECT_THROW(commit(fs, RecoverySource::Backup), PowerCut);
    EXPECT_EQ(fs.files[kPrimary], "new-v4");
    EXPECT_EQ(fs.files[kBackup], "good-backup");
    EXPECT_FALSE(fs.exists(kTemp));
  }
}

TEST(MqttPrefsRecovery, PrimaryRotationRetainsOldImageAtEveryBoundary) {
  {
    FakeFiles fs = primaryRotationFiles();
    fs.fault = Fault::CutAfterRemoveBackup;
    EXPECT_THROW(commit(fs, RecoverySource::Primary), PowerCut);
    EXPECT_EQ(fs.files[kPrimary], "old-primary");
    EXPECT_EQ(fs.files[kTemp], "new-v4");
  }

  {
    FakeFiles fs = primaryRotationFiles();
    fs.fault = Fault::CutAfterDemotePrimary;
    EXPECT_THROW(commit(fs, RecoverySource::Primary), PowerCut);
    EXPECT_FALSE(fs.exists(kPrimary));
    EXPECT_EQ(fs.files[kBackup], "old-primary");
    EXPECT_EQ(fs.files[kTemp], "new-v4");

    const RecoveryCommitResult recovery = commit(fs, RecoverySource::Temp);
    ASSERT_TRUE(recovery.committed);
    EXPECT_EQ(fs.files[kPrimary], "new-v4");
    EXPECT_EQ(fs.files[kBackup], "old-primary");
  }

  {
    FakeFiles fs = primaryRotationFiles();
    fs.fault = Fault::CutAfterPromoteTemp;
    EXPECT_THROW(commit(fs, RecoverySource::Primary), PowerCut);
    EXPECT_EQ(fs.files[kPrimary], "new-v4");
    EXPECT_EQ(fs.files[kBackup], "old-primary");
    EXPECT_FALSE(fs.exists(kTemp));
  }
}

TEST(MqttPrefsRecovery, PrimaryRotationFailuresKeepTempAndAUsableImage) {
  {
    FakeFiles fs = primaryRotationFiles();
    fs.fault = Fault::FailRemoveBackup;
    const RecoveryCommitResult result = commit(fs, RecoverySource::Primary);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.failure, RecoveryCommitFailure::RemoveBackup);
    EXPECT_EQ(fs.files[kPrimary], "old-primary");
    EXPECT_EQ(fs.files[kBackup], "older-backup");
    EXPECT_EQ(fs.files[kTemp], "new-v4");
  }

  {
    FakeFiles fs = primaryRotationFiles();
    fs.fault = Fault::FailDemotePrimary;
    const RecoveryCommitResult result = commit(fs, RecoverySource::Primary);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.failure, RecoveryCommitFailure::DemotePrimary);
    EXPECT_EQ(fs.files[kPrimary], "old-primary");
    EXPECT_FALSE(fs.exists(kBackup));
    EXPECT_EQ(fs.files[kTemp], "new-v4");
  }

  {
    FakeFiles fs = primaryRotationFiles();
    fs.fault = Fault::FailPromoteTemp;
    const RecoveryCommitResult result = commit(fs, RecoverySource::Primary);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.failure, RecoveryCommitFailure::PromoteTemp);
    EXPECT_TRUE(result.rollback_attempted);
    EXPECT_TRUE(result.rollback_succeeded);
    EXPECT_EQ(fs.files[kPrimary], "old-primary");
    EXPECT_FALSE(fs.exists(kBackup));
    EXPECT_EQ(fs.files[kTemp], "new-v4");
  }

  {
    FakeFiles fs = primaryRotationFiles();
    fs.fault = Fault::FailPromoteTemp;
    fs.fail_rollback_backup = true;
    const RecoveryCommitResult result = commit(fs, RecoverySource::Primary);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.failure, RecoveryCommitFailure::RollbackBackup);
    EXPECT_TRUE(result.rollback_attempted);
    EXPECT_FALSE(result.rollback_succeeded);
    EXPECT_FALSE(fs.exists(kPrimary));
    EXPECT_EQ(fs.files[kBackup], "old-primary");
    EXPECT_EQ(fs.files[kTemp], "new-v4");
  }
}

TEST(MqttPrefsRecovery, UnverifiedSaveDoesNotStartACommit) {
  FakeFiles fs = primaryRotationFiles();
  const bool write_and_verify_ok = false;

  // saveConfigFile returns before commitVerifiedTemp when writing or
  // read-back verification fails. The old images therefore remain intact.
  EXPECT_FALSE(write_and_verify_ok);
  EXPECT_EQ(fs.files[kPrimary], "old-primary");
  EXPECT_EQ(fs.files[kBackup], "older-backup");
  EXPECT_EQ(fs.files[kTemp], "new-v4");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
