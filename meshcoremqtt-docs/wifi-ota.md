# Wi-Fi OTA Updates

`start ota` opens a temporary, isolated Wi-Fi OTA window so an installed repeater can be updated without USB access.

## Availability

Wi-Fi OTA is compiled into release builds for boards with 16 MB or more of flash (for example Heltec V4, LilyGO T-Deck, Station G2/G3, ThinkNode M9). Builds that do not include it reply:

```text
Error: OTA not supported in this build
```

Boards with 8 MB of flash (for example Heltec V3) cannot hold the firmware image and an OTA slot at the same time; update those over USB.

## Starting a window

1. Open the device's serial console and send `start ota`.
2. The firmware first stops mesh and MQTT activity behind a verified stop gate. If the reporter cannot confirm a clean stop, the request is refused and nothing changes.
3. The device raises an isolated access point named `MeshCore-OTA`. Station Wi-Fi is switched off for the window, so the update page cannot be reached from the site LAN.
4. The serial reply prints the session URL and generated credentials, for example:

   ```text
   Started: http://192.168.4.1/update AP-pass=<generated> HTTP=<user>:<generated> (10 min)
   ```

   The AP password and HTTP credentials are generated per session and are only available from this reply.
5. Join `MeshCore-OTA`, open `http://192.168.4.1/update`, authenticate with the printed HTTP credentials, and upload the `...-update.bin` asset for your board from the release.
6. The upload writes the inactive application slot and the device reboots into the new image. Mesh and MQTT resume automatically.

If nothing is uploaded, the window closes after 10 minutes and the device returns to normal operation. Only one OTA window can be opened per boot: sending `start ota` again without rebooting replies `Error: reboot before starting OTA again` — reboot the device to open a new window.

## Failure replies

- `Error: OTA not supported in this build` — the build was compiled without Wi-Fi OTA.
- `Error: OTA already active` — a window is currently open; use it or wait for it to expire.
- `Error: reboot before starting OTA again` — a window has already been used since this boot; reboot the device first.
- `Error: MQTT stop unverified; OTA flash refused` — the reporter did not verify a clean stop; the flash gate stayed closed for safety.
- `Error: could not start isolated OTA access point` — the access point could not be raised; the device stays on its previous Wi-Fi mode.

## Notes

- The update page and the `/log` view require the session HTTP credentials; unauthenticated requests are rejected.
- After a successful upload the device boots from the newly written slot; a restart at this point is expected.
