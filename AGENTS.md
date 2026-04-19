# Agent Memory

- Before pushing anything to GitHub, remind Ben to run the MQTT firmware release
  artifact script with an explicit version marker:
  `bin/prepare-mqtt-firmware-release.sh --version <version> --branch <dev|main> [env ...]`
- Wait for the script to finish successfully. It builds the `.bin` artifacts,
  writes `webflasher/<branch>/release.json`, embeds the MQTT client version
  marker, and stages the generated `webflasher/<branch>` files.
- Publish the same firmware bundle under the root-level `MQTT_Firmware/<branch>`
  folder before pushing. If needed, run:
  `bin/sync-mqtt-firmware-folder.sh <dev|main>`
- Review and commit the staged firmware artifacts and version markers before
  running `git push`.
