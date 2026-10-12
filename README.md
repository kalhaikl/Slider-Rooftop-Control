<div align="center">

# 🏠 ATAP Controller

**Kontrol 2 atap/kanopi motor berbasis remote RF lewat dashboard web dan MQTT, dengan ESP8266 (Wemos D1 mini).**

![Platform](https://img.shields.io/badge/platform-ESP8266-blue)
![Framework](https://img.shields.io/badge/framework-Arduino-00979D)
![MQTT](https://img.shields.io/badge/protocol-MQTT-660066)
![OTA](https://img.shields.io/badge/update-OTA-success)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

[Fitur](#-fitur) · [Perangkat Keras](#-perangkat-keras) · [Instalasi](#-instalasi) · [Setup](#-konfigurasi-pertama-kali) · [MQTT](#-mqtt) · [Kode RF](#-mengganti-kode-rf) · [Kalibrasi](#-kalibrasi-waktu-gerak)

</div>

---

## 📖 Tentang

**ATAP Controller** menggantikan remote RF atap/kanopi motor dengan sebuah ESP8266. Perangkat ini **mengirim ulang (replay) kode RF** dari remote asli lewat modul transmitter, sehingga atap bisa dibuka, ditutup, dan dihentikan dari HP, browser, atau sistem smart home lewat MQTT.

Karena motor atap umumnya tidak punya sensor posisi, persentase bukaan **diestimasi dari lamanya waktu gerak**. Dengan begitu atap bisa diatur ke posisi 25%, 50%, atau 75% dan statusnya tetap tampil di dashboard serta dikirim ke MQTT.

Firmware ini mendukung **2 atap** sekaligus (Atap 1 dan Atap 2), masing-masing dengan kode RF, status, dan topik MQTT sendiri.

## ✨ Fitur

- 🎛️ **Kontrol 2 atap** — buka penuh, tutup penuh, preset 25 / 50 / 75%, pause, dan "pause semua"
- 📊 **Estimasi posisi** berdasarkan waktu gerak, lengkap dengan progress bar animasi di dashboard
- 📡 **MQTT** dengan LWT (Online/Offline), status *retained*, dan dukungan login
- 🌐 **Dashboard web mobile-first** dengan tema terang/gelap otomatis, tanpa CDN (jalan offline)
- 🛠️ **Pengaturan lewat web** — Wi-Fi, broker MQTT, dan base topic tersimpan di EEPROM
- 📶 **Access Point otomatis** — menyala sendiri jika Wi-Fi belum diatur atau putus, mati sendiri saat Wi-Fi stabil
- 🔄 **OTA update** lewat halaman `/update`
- 📻 **Pengiriman RF stabil** — timing berbasis *cycle counter* CPU, interrupt dikunci saat kirim frame, 8 frame per perintah, dan jeda minimum antar pengiriman
- ⚡ **Latensi rendah** — Wi-Fi modem sleep dimatikan agar perintah MQTT/HTTP langsung diproses
- 🔁 **Auto reconnect** Wi-Fi dan MQTT

## 🔧 Perangkat Keras

| Komponen | Keterangan |
| --- | --- |
| ESP8266 (Wemos D1 mini / NodeMCU) | Mikrokontroler utama |
| Modul transmitter RF | Biasanya 433 MHz, sesuaikan dengan frekuensi remote atap Anda |
| Remote RF atap asli | Sumber kode RF yang akan ditiru |
| Antena (opsional) | Kabel ±17 cm untuk 433 MHz meningkatkan jangkauan |

### Wiring

| Modul RF | ESP8266 |
| --- | --- |
| `DATA` | `D5` (GPIO14) |
| `VCC` | Sesuai spesifikasi modul (3V3 atau 5V) |
| `GND` | `GND` |

Pin bisa diubah di bagian atas kode:

```cpp
#define RF_PIN D5
```

> ⚠️ Kode pengiriman menggunakan akses register GPIO langsung (`GPOS`/`GPOC`), jadi gunakan pin GPIO0–GPIO15 (D0/GPIO16 tidak didukung).

## 📦 Instalasi

### Prasyarat

- [Arduino IDE](https://www.arduino.cc/en/software) atau PlatformIO
- Dukungan board ESP8266 (ESP8266 Arduino core)

### Library

| Library | Keterangan |
| --- | --- |
| `PubSubClient` | Klien MQTT |
| `ESP8266WiFi`, `ESP8266WebServer`, `ESP8266HTTPUpdateServer`, `EEPROM` | Sudah termasuk di core ESP8266 |

Contoh `platformio.ini`:

```ini
[env:d1_mini]
platform = espressif8266
board = d1_mini
framework = arduino
monitor_speed = 115200
lib_deps =
    knolleary/PubSubClient
```

### Upload

```bash
git clone https://github.com/USERNAME/REPO.git
cd REPO
pio run -t upload
pio device monitor
```

Atau buka file `.ino` di Arduino IDE, pilih board **LOLIN(WEMOS) D1 R2 & mini**, lalu **Upload**. Upload pertama harus lewat USB; update berikutnya bisa lewat OTA.

## ⚙️ Konfigurasi Pertama Kali

1. Nyalakan perangkat. Karena Wi-Fi belum diatur, ESP8266 membuat Access Point:
   - **SSID:** `ATAP-SETUP`
   - **Password:** *(tanpa password)*
2. Sambungkan HP/laptop ke AP tersebut, lalu buka `http://192.168.4.1`.
3. Buka **Wi-Fi dan MQTT** (`/config`), isi SSID dan password router, host dan port broker MQTT, login (jika ada), serta base topic.
4. Klik **Simpan konfigurasi**. Perangkat akan langsung menghubungkan Wi-Fi dan MQTT.
5. Cek alamat IP baru di Serial Monitor, router, atau topik MQTT `<base>/IP`, lalu buka dashboard.

**Perilaku Access Point otomatis**

| Kondisi | Aksi |
| --- | --- |
| Wi-Fi belum diatur | AP selalu menyala |
| Wi-Fi gagal saat boot / putus > 20 detik | AP menyala |
| Wi-Fi stabil > 30 detik dan tidak ada perangkat tersambung ke AP | AP mati |

Selama ada perangkat yang tersambung ke AP, perangkat tidak melakukan reconnect Wi-Fi agar sesi konfigurasi tidak terputus.

### Endpoint web

| Alamat | Fungsi |
| --- | --- |
| `/` | Dashboard kontrol |
| `/config` | Pengaturan Wi-Fi dan MQTT |
| `/status` | Status perangkat dan atap (JSON) |
| `/cmd?r=1&c=open` | Kirim perintah ke atap |
| `/update` | OTA update firmware |

Parameter `/cmd`: `r` = `1`, `2`, atau `all`; `c` = perintah (lihat [daftar perintah](#perintah)).

```bash
# Buka penuh Atap 1
curl "http://192.168.1.50/cmd?r=1&c=open"

# Pause semua atap
curl "http://192.168.1.50/cmd?r=all&c=pause"
```

## 📡 MQTT

Base topic bawaan: `ATAP` (bisa diubah di `/config`; tanpa spasi, `/`, `+`, atau `#`). Topik MQTT peka huruf besar/kecil.

| Topik | Arah | Isi |
| --- | --- | --- |
| `ATAP/LWT` | publish | `Online` / `Offline` (retained) |
| `ATAP/IP` | publish | Alamat IP perangkat (retained) |
| `ATAP/Data/atap1/stat` | publish | Status Atap 1 (retained) |
| `ATAP/Data/atap1/cmnd` | subscribe | Perintah untuk Atap 1 |
| `ATAP/Data/atap2/stat` | publish | Status Atap 2 (retained) |
| `ATAP/Data/atap2/cmnd` | subscribe | Perintah untuk Atap 2 |

### Perintah

Kirim ke topik `.../cmnd` (tidak peka huruf besar/kecil):

| Payload | Aksi |
| --- | --- |
| `open`, `on`, `1` | Buka penuh |
| `close`, `off`, `0` | Tutup penuh |
| `pause`, `stop` | Hentikan gerakan |
| `25`, `50`, `75` | Pindah ke posisi tersebut (butuh posisi yang sudah diketahui) |
| `nothing`, `c` | Kirim kode RF "nothing"/lock (kode C) |

### Status

Isi topik `.../stat`:

| Status | Arti |
| --- | --- |
| `terbuka` | Terbuka penuh (100%) |
| `tertutup` | Tertutup penuh (0%) |
| `0%`–`100%` | Posisi estimasi saat bergerak atau berhenti di tengah |
| `membuka` / `menutup` | Sedang bergerak, tetapi posisi belum diketahui |
| `pause` | Dihentikan di tengah gerakan |
| `unknown` | Posisi belum diketahui |

### Perilaku topik `cmnd`

Setiap kali terhubung ke broker, perangkat mengisi topik `cmnd` dengan **satu spasi** (retained) sebelum subscribe. Tujuannya:

- topik langsung muncul di broker (payload yang benar-benar kosong justru menghapus topik retained),
- perintah retained lama **tidak dieksekusi ulang** saat perangkat reconnect.

Payload kosong atau spasi diabaikan sebagai perintah.

Contoh dengan Mosquitto:

```bash
# Pantau semua topik
mosquitto_sub -h 192.168.1.100 -t "ATAP/#" -v

# Buka Atap 1 penuh
mosquitto_pub -h 192.168.1.100 -t "ATAP/Data/atap1/cmnd" -m "open"

# Atap 2 ke 50%
mosquitto_pub -h 192.168.1.100 -t "ATAP/Data/atap2/cmnd" -m "50"
```

### Contoh Home Assistant

```yaml
mqtt:
  cover:
    - name: "Atap 1"
      command_topic: "ATAP/Data/atap1/cmnd"
      state_topic: "ATAP/Data/atap1/stat"
      payload_open: "open"
      payload_close: "close"
      payload_stop: "pause"
      state_open: "terbuka"
      state_closed: "tertutup"
      state_opening: "membuka"
      state_closing: "menutup"
      state_stopped: "pause"
      availability_topic: "ATAP/LWT"
      payload_available: "Online"
      payload_not_available: "Offline"
```

> Status persentase di tengah gerak (mis. `45%`) tidak dipetakan ke state standar cover Home Assistant. Untuk menampilkannya, tambahkan juga entitas `sensor` pada topik `stat` yang sama.

## 📻 Mengganti Kode RF

Kode RF dalam source code adalah **salinan dari remote milik penulis** dan hampir pasti tidak cocok dengan atap Anda. Anda perlu merekam sinyal remote sendiri, lalu mengganti string bit-nya.

Setiap atap memiliki 4 kode:

| Kode | Fungsi | Variabel (Atap 1 / Atap 2) |
| --- | --- | --- |
| A | OPEN (buka) | `RF1_CODE_A_BITS` / `RF2_CODE_A_BITS` |
| B | CLOSE (tutup) | `RF1_CODE_B_BITS` / `RF2_CODE_B_BITS` |
| C | NOTHING (mis. tombol kunci) | `RF1_CODE_C_BITS` / `RF2_CODE_C_BITS` |
| D | PAUSE (stop) | `RF1_CODE_D_BITS` / `RF2_CODE_D_BITS` |

Satu karakter `'1'` atau `'0'` mewakili level pin selama `RF_BIT_US` (300 µs):

```cpp
static const char RF1_CODE_A_BITS[] = "1111010000111101000010000111...";
```

Cara merekam kode:

1. Rekam sinyal tiap tombol remote dengan RTL-SDR (mis. dengan Universal Radio Hacker) atau logic analyzer yang dipasang pada modul RF remote.
2. Ukur durasi satu bit dasar, lalu sesuaikan `RF_BIT_US`.
3. Ukur jeda antar-frame dan sesuaikan `RF_GAP_US`.
4. Ubah sinyal menjadi string `1`/`0` per slot waktu dasar dan tempel ke variabel di atas.

Parameter pengiriman:

```cpp
#define RF_BIT_US 300           // durasi satu bit (µs)
#define RF_GAP_US 10836         // jeda antar-frame (µs)
#define RF_SEND_COUNT 8         // jumlah frame per perintah
#define RF_MIN_INTERVAL_MS 150  // jeda minimum antar pengiriman
#define RF_LOCK_INTERRUPTS 1    // 1 = matikan interrupt saat kirim frame
```

Jika atap kadang tidak merespons, coba naikkan `RF_SEND_COUNT`. Jika Wi-Fi sering terputus saat pengiriman, coba set `RF_LOCK_INTERRUPTS` ke `0`.

## ⏱️ Kalibrasi Waktu Gerak

Posisi dihitung dari lama gerak, sehingga waktu penuh harus sesuai dengan atap Anda:

```cpp
// Durasi gerak penuh (ms): {Atap 1, Atap 2}
const unsigned long OPEN_FULL_MS[ROOF_COUNT]  = {5000, 5000};
const unsigned long CLOSE_FULL_MS[ROOF_COUNT] = {5000, 5000};
```

1. Ukur dengan stopwatch berapa detik atap bergerak dari tertutup penuh sampai terbuka penuh (dan sebaliknya).
2. Isi nilainya dalam milidetik, lalu upload ulang.
3. Setelah itu, jalankan **Buka penuh** atau **Tutup penuh** sekali agar posisi referensi diketahui.

> 📝 **Catatan:** Tanpa sensor posisi, estimasi bisa bergeser seiring waktu atau jika atap digerakkan dengan remote asli. Jalankan buka/tutup penuh secara berkala untuk menyelaraskan ulang. Sebelum posisi diketahui, preset 25/50/75% dinonaktifkan.

## 🔬 Cara Kerja Singkat

```
Perintah (Web / MQTT)
        │
        ▼
 handleCommand ──► moveToPosition ──► hitung durasi dari posisi & target
        │                                   │
        │                                   ▼
        │                          kirim kode RF (A/B) ──► atap bergerak
        │                                   │
        ▼                                   ▼
 updatePosition  ◄── timer ──── selesai? ──► kirim kode RF D (pause) ──► posisi = target
        │
        ▼
 publish status ke MQTT & dashboard
```

Konfigurasi (Wi-Fi, MQTT, base topic) disimpan di EEPROM dengan penanda `CONFIG_MAGIC`. Jika data tidak valid, nilai default dipakai (broker `192.168.1.100`, port `1883`, base topic `ATAP`).

## 🔐 Catatan Keamanan

Perangkat ini mengendalikan peralatan fisik, jadi perhatikan hal berikut:

- Dashboard, `/cmd`, dan `/update` **tidak memiliki autentikasi**. Siapa pun di jaringan yang sama bisa menggerakkan atap atau memasang firmware. Gunakan hanya di jaringan lokal tepercaya
- Access Point setup **tidak memakai password**; isi `AP_PASSWORD` (minimal 8 karakter) jika perlu
- Password Wi-Fi dan MQTT disimpan sebagai teks biasa di EEPROM
- Pastikan area gerak atap bebas dari orang, hewan, atau benda saat dikendalikan dari jarak jauh. Tombol **Pause** tersedia di dashboard untuk menghentikan gerakan
- Karena posisi hanya estimasi, jangan mengandalkan perangkat ini sebagai satu-satunya pengaman (gunakan limit switch bawaan motor)

## 🗺️ Ide Pengembangan

- [ ] Autentikasi untuk dashboard, `/cmd`, dan OTA
- [ ] MQTT Discovery untuk Home Assistant
- [ ] Sensor limit switch atau sensor hujan untuk penutupan otomatis
- [ ] Penjadwalan buka/tutup
- [ ] Jumlah atap yang bisa diatur lewat web

## 🤝 Kontribusi

Kontribusi, saran, dan laporan bug sangat diterima. Silakan buat *issue* atau kirim *pull request*.

1. Fork repositori ini
2. Buat branch fitur (`git checkout -b fitur/nama-fitur`)
3. Commit perubahan (`git commit -m 'Tambah fitur'`)
4. Push ke branch (`git push origin fitur/nama-fitur`)
5. Buka Pull Request

## 📄 Lisensi

Didistribusikan di bawah lisensi MIT. Lihat [LICENSE](LICENSE) untuk detail.

---

<div align="center">

Jika proyek ini bermanfaat, jangan lupa beri ⭐

</div>
