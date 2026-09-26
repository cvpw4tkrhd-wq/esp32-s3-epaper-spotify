# ESP32-S3-ePaper-1.54: Spotify "spelas nu"

Visar låt, artist, albumomslag och förlopp för det som spelas på ditt Spotify-konto, på Waveshare **ESP32-S3-ePaper-1.54**
(1,54" svartvitt e-papper, 200x200). Albumomslaget avkodas till gråskala och dithras till svartvitt (Floyd–Steinberg).

| Knapp | Funktion |
|---|---|
| **BOOT** (GPIO0) | start / stopp |
| **PWR** (GPIO18) | nästa låt |

Styrknapparna kräver **Spotify Premium**. Att bara visa vad som spelas fungerar även utan.
Skärmen uppdateras helt vid låtbyte och delvis (förloppsstapeln) var 15:e sekund. Titlar med kinesiska, japanska och
kyrilliska tecken visas med ett bredare typsnitt.

## Uppsättning

1. **Skapa en Spotify-app** på <https://developer.spotify.com/dashboard> (välj *Web API*). Lägg till redirect-URI
   `http://127.0.0.1:8888/callback` (exakt så, inte `localhost`) och kopiera **Client ID** (32 tecken). Appen körs i
   utvecklarläge: upp till 5 användare och ägaren behöver Premium.
2. **Fyll i WiFi:** `cp secrets.h.example secrets.h` och ändra `WIFI_SSID` / `WIFI_PASS` (2,4 GHz).
3. **Logga in en gång på datorn.** Skriptet öppnar Spotify i webbläsaren, du godkänner själv, och refresh-token skrivs
   direkt till `secrets.h` (visas aldrig i terminalen):

   ```bash
   python3 spotify_auth.py --client-id DITT_CLIENT_ID
   ```

4. **Bygg och flasha** (arduino-cli):

   ```bash
   arduino-cli core install esp32:esp32
   arduino-cli lib install ArduinoJson JPEGDEC U8g2_for_Adafruit_GFX GxEPD2 "Adafruit GFX Library"
   arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=cdc" --build-path build .
   esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x0 build/esp32-s3-epaper-spotify.ino.merged.bin
   ```

   Om kortet inte går att flasha: håll **BOOT** nedtryckt medan USB-kabeln sätts i (nedladdningsläge, `303a:1001`),
   och starta sedan om utan BOOT.

Ändrar du `secrets.h` (till exempel efter ny inloggning) läser kortet in den nya token vid nästa flashning.

## Hur det fungerar

- Inloggning: Authorization Code med PKCE (ingen client secret behövs). Kortet förnyar access-token med refresh-token.
- Var 5:e sekund (10 s när inget spelar) frågar kortet `GET /v1/me/player/currently-playing`. Knapparna anropar
  `PUT /v1/me/player/play|pause` och `POST /v1/me/player/next`.
- Omslaget hämtas i 300 px, avkodas i halv storlek (150x150) med JPEGDEC, kontraststräcks och dithras.
- Anslutningarna använder TLS utan certifikatkontroll (`setInsecure`), en förenkling för ett hemmabygge.

## Pinnar (ESP32-S3-ePaper-1.54)

| Funktion | GPIO |
|---|---|
| e-papper DC / CS / SCK / MOSI / RST / BUSY | 10 / 11 / 12 / 13 / 9 / 8 |
| e-papper ström (aktiv låg) | 6 |
| batteriväg (aktiv hög) | 17 |
| knappar BOOT / PWR | 0 / 18 |

## Licens

GNU GPL v3 (se `LICENSE`), eftersom programmet använder GxEPD2 (GPL-3.0). Övriga bibliotek: ArduinoJson (MIT),
JPEGDEC (Apache-2.0), U8g2 for Adafruit GFX (BSD), Adafruit GFX (BSD). Spotify är ett varumärke som tillhör Spotify AB;
projektet är inte kopplat till eller godkänt av Spotify.
