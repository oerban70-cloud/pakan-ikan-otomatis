// PakanIkanOtomatis — Pakan ikan otomatis pakai ESP32 + RTC DS3231 + servo SG90
// ---------------------------------------------------------------------------
// Ikan di akuarium diberi makan otomatis dua kali sehari, pagi dan sore,
// tanpa pemilik harus ada di rumah. Jam RTC DS3231 menyimpan jadwal dan
// tetap berjalan walau ESP32 sempat mati (baterai CR2032 di modul RTC).
// Begitu waktu makan tiba, servo SG90 memiringkan wadah pakan sebentar
// supaya pakan jatuh secukupnya ke akuarium, lalu wadah kembali menutup.
// Tombol manual memberi makan sekali di luar jadwal (mis. saat pulang
// dan ingin memberi makan tambahan). LED hijau menyala selama servo
// membuka wadah, jadi pemilik tahu pakan sedang keluar.
//
// Cara pakai:
//   - Isi wadah pakan dengan pelet ikan secukupnya, pasang wadah di atas
//     akuarium dengan lubang keluar menghadap ke air.
//   - Atur jam RTC sekali lewat Serial Monitor (ketik 'j' lalu Enter,
//     lalu ikuti petunjuk: masukkan tanggal dan jam, mis. 2026-11-22 19:30).
//   - Atur jadwal makan dengan mengubah JADWAL_MAKAN di bawah ini.
//   - Perintah Serial Monitor (115200 baud):
//       ketik 'm' lalu <Enter>  ->  beri makan sekali (manual)
//       ketik 's' lalu <Enter>  ->  tampilkan status dan jadwal
//       ketik 'j' lalu <Enter>  ->  atur jam RTC (format YYYY-MM-DD HH:MM)
//
// Wiring (lihat schematic.png):
//   RTC DS3231 (I2C)
//       SDA -> GPIO21, SCL -> GPIO22, VCC -> 3V3, GND -> GND
//   Servo SG90
//       SIG (oranye) -> GPIO13, VCC (merah) -> 5V (VIN), GND (coklat) -> GND
//   Tombol manual (push button, aktif LOW)
//       kaki 1 -> GPIO15, kaki 2 -> GND (pakai INPUT_PULLUP, tanpa resistor luar)
//   LED hijau (seri resistor 220 ohm)
//       anoda -> GPIO33, katoda -> GND
//   ESP32 dicatu USB 5V; servo 5V diambil dari pin VIN (satu sumber 5V
//   yang sama, tapi arusnya lewat servo langsung, bukan lewat pin GPIO).
//
// Library yang dipakai: RTClib 2.1.4, ESP32Servo 3.2.1.
// ---------------------------------------------------------------------------

#include <RTClib.h>
#include <ESP32Servo.h>

// ------------------------- Konfigurasi -------------------------------------
// Pin sensor dan aktuator.
constexpr uint8_t PIN_SERVO  = 13;  // SIG servo SG90 (PWM 50 Hz)
constexpr uint8_t PIN_TOMBOL = 15;  // tombol manual, aktif LOW (INPUT_PULLUP)
constexpr uint8_t PIN_LED    = 33;  // LED hijau, aktif HIGH

// Pin I2C RTC DS3231 (pin default ESP32).
constexpr uint8_t PIN_SDA = 21;
constexpr uint8_t PIN_SCL = 22;

// Posisi servo (derajat).
constexpr int SUDUT_TUTUP = 0;    // wadah tertutup
constexpr int SUDUT_BUKA  = 90;   // wadah miring, pakan jatuh

// Waktu dan ambang.
constexpr uint32_t WAKTU_BUKA_MS     = 1500;  // servo terbuka tiap pemberian pakan
constexpr uint32_t INTERVAL_CEK_MS   = 1000;  // cek jam tiap 1 detik
constexpr uint32_t DEBOUNCE_TOMBOL_MS = 50;   // debounce tombol manual
constexpr uint32_t BAUD_SERIAL       = 115200;

// Jadwal makan (jam, menit). Ubah di sini untuk mengganti jadwal.
struct JadwalMakan {
  uint8_t jam;
  uint8_t menit;
};
constexpr JadwalMakan JADWAL_MAKAN[] = {
    {7, 0},   // pagi jam 07:00
    {17, 0},  // sore jam 17:00
};
constexpr uint8_t JUMLAH_JADWAL =
    sizeof(JADWAL_MAKAN) / sizeof(JADWAL_MAKAN[0]);

// ------------------------- Status ------------------------------------------
RTC_DS3231 rtc;
Servo servoPakan;

// State pemberian pakan: false = wadah tertutup, true = sedang memberi makan.
bool sedangMemberiMakan = false;
uint32_t waktuMakanMulai = 0;  // millis() saat servo dibuka
uint32_t waktuCekTerakhir = 0;  // millis() cek jadwal terakhir
uint32_t jumlahPemberian = 0;  // penghitung pemberian pakan sejak dinyalakan

// Mencegah pemberian pakan berulang di menit yang sama: simpan menit
// terakhir tiap jadwal sudah dijalankan (dalam menit sejak tengah malam).
int16_t menitTerakhirDijalankan[JUMLAH_JADWAL];

// State debounce tombol manual.
bool tombolTerakhir = HIGH;
uint32_t waktuTombolBerubah = 0;

// ------------------------- Fungsi ------------------------------------------
// Buka wadah pakan: servo ke SUDUT_BUKA, LED hijau ON.
void mulaiMemberiMakan(const char* sumber) {
  servoPakan.write(SUDUT_BUKA);
  digitalWrite(PIN_LED, HIGH);
  sedangMemberiMakan = true;
  waktuMakanMulai = millis();
  jumlahPemberian++;
  Serial.print("Pemberian pakan #");
  Serial.print(jumlahPemberian);
  Serial.print(" (");
  Serial.print(sumber);
  Serial.println(") — wadah terbuka...");
}

// Tutup wadah pakan: servo kembali ke SUDUT_TUTUP, LED hijau OFF.
void akhiriMemberiMakan() {
  servoPakan.write(SUDUT_TUTUP);
  digitalWrite(PIN_LED, LOW);
  sedangMemberiMakan = false;
  Serial.println("Wadah tertutup, pakan sudah jatuh.");
}

// Cek apakah sekarang cocok dengan salah satu jadwal makan, dan jadwal
// itu belum dijalankan di menit ini. Mengembalikan indeks jadwal atau -1.
int8_t cekJadwalMakan(const DateTime& sekarang) {
  const int16_t menitSekarang = sekarang.hour() * 60 + sekarang.minute();
  for (uint8_t i = 0; i < JUMLAH_JADWAL; i++) {
    const int16_t menitJadwal =
        JADWAL_MAKAN[i].jam * 60 + JADWAL_MAKAN[i].menit;
    if (menitSekarang == menitJadwal &&
        menitTerakhirDijalankan[i] != menitJadwal) {
      menitTerakhirDijalankan[i] = menitJadwal;
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

// Tampilkan status alat, jam RTC, dan jadwal makan di Serial Monitor.
void tampilkanStatus() {
  const DateTime sekarang = rtc.now();
  Serial.println("--- Status pakan ikan otomatis ---");
  Serial.print("Jam RTC: ");
  Serial.print(sekarang.year());
  Serial.print('-');
  Serial.print(sekarang.month());
  Serial.print('-');
  Serial.print(sekarang.day());
  Serial.print(' ');
  Serial.print(sekarang.hour());
  Serial.print(':');
  if (sekarang.minute() < 10) {
    Serial.print('0');
  }
  Serial.println(sekarang.minute());
  Serial.print("Wadah: ");
  Serial.println(sedangMemberiMakan ? "terbuka (memberi makan)" : "tertutup");
  Serial.print("Jumlah pemberian pakan: ");
  Serial.println(jumlahPemberian);
  Serial.println("Jadwal makan:");
  for (uint8_t i = 0; i < JUMLAH_JADWAL; i++) {
    Serial.print("  - ");
    Serial.print(JADWAL_MAKAN[i].jam);
    Serial.print(':');
    if (JADWAL_MAKAN[i].menit < 10) {
      Serial.print('0');
    }
    Serial.println(JADWAL_MAKAN[i].menit);
  }
}

// Atur jam RTC dari teks "YYYY-MM-DD HH:MM" yang diketik di Serial Monitor.
void aturJamRtc(const String& teks) {
  const int tahun = teks.substring(0, 4).toInt();
  const int bulan = teks.substring(5, 7).toInt();
  const int tanggal = teks.substring(8, 10).toInt();
  const int jam = teks.substring(11, 13).toInt();
  const int menit = teks.substring(14, 16).toInt();
  if (tahun < 2020 || bulan < 1 || bulan > 12 || tanggal < 1 ||
      tanggal > 31 || jam > 23 || menit > 59) {
    Serial.println("Format salah. Pakai YYYY-MM-DD HH:MM, mis. 2026-11-22 19:30");
    return;
  }
  rtc.adjust(DateTime(tahun, bulan, tanggal, jam, menit, 0));
  Serial.print("Jam RTC diatur ke ");
  Serial.println(teks);
}

// Tangani satu perintah dari Serial Monitor.
// 'm' = beri makan sekali, 's' = status, 'j' = atur jam RTC.
void tanganiPerintahSerial(char perintah) {
  switch (perintah) {
    case 'm':
    case 'M':
      if (!sedangMemberiMakan) {
        Serial.println("Pemberian pakan manual dari Serial.");
        mulaiMemberiMakan("manual Serial");
      } else {
        Serial.println("Wadah sedang terbuka, tunggu sampai tertutup.");
      }
      break;
    case 's':
    case 'S':
      tampilkanStatus();
      break;
    case 'j':
    case 'J':
      Serial.println("Masukkan tanggal dan jam (YYYY-MM-DD HH:MM), lalu Enter:");
      break;
    default:
      Serial.println("Perintah tidak dikenal. 'm' = makan manual, "
                     "'s' = status, 'j' = atur jam.");
      break;
  }
}

// ------------------------- setup / loop ------------------------------------
void setup() {
  Serial.begin(BAUD_SERIAL);
  Wire.begin(PIN_SDA, PIN_SCL);

  pinMode(PIN_TOMBOL, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  if (!rtc.begin()) {
    Serial.println("RTC DS3231 tidak ditemukan! Periksa wiring SDA/SCL.");
    while (true) {
      delay(1000);
    }
  }
  if (rtc.lostPower()) {
    Serial.println("RTC kehilangan daya, jam diatur ke waktu compile.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  servoPakan.setPeriodHertz(50);
  servoPakan.attach(PIN_SERVO, 500, 2400);
  servoPakan.write(SUDUT_TUTUP);

  for (uint8_t i = 0; i < JUMLAH_JADWAL; i++) {
    menitTerakhirDijalankan[i] = -1;
  }

  Serial.println("Pakan ikan otomatis siap.");
  Serial.println("Ikan diberi makan otomatis jam 07:00 dan 17:00.");
  Serial.println("Ketik 'm' = makan manual, 's' = status, 'j' = atur jam.");
  tampilkanStatus();
}

void loop() {
  const uint32_t sekarang = millis();

  // Cek jadwal tiap INTERVAL_CEK_MS (bukan tiap loop, supaya stabil).
  if (sekarang - waktuCekTerakhir >= INTERVAL_CEK_MS) {
    waktuCekTerakhir = sekarang;

    if (!sedangMemberiMakan) {
      const DateTime waktu = rtc.now();
      const int8_t indeksJadwal = cekJadwalMakan(waktu);
      if (indeksJadwal >= 0) {
        Serial.print("Jadwal makan tiba (");
        Serial.print(JADWAL_MAKAN[indeksJadwal].jam);
        Serial.print(':');
        Serial.print(JADWAL_MAKAN[indeksJadwal].menit);
        Serial.println(").");
        mulaiMemberiMakan("jadwal");
      }
    }
  }

  // Tutup wadah setelah WAKTU_BUKA_MS, tanpa menahan loop.
  if (sedangMemberiMakan &&
      (sekarang - waktuMakanMulai >= WAKTU_BUKA_MS)) {
    akhiriMemberiMakan();
  }

  // Tombol manual (aktif LOW) dengan debounce: tekan sekali = makan sekali.
  const bool tombolSekarang = digitalRead(PIN_TOMBOL);
  if (tombolSekarang != tombolTerakhir) {
    waktuTombolBerubah = sekarang;
    tombolTerakhir = tombolSekarang;
  }
  if (tombolSekarang == LOW && !sedangMemberiMakan &&
      (sekarang - waktuTombolBerubah >= DEBOUNCE_TOMBOL_MS)) {
    Serial.println("Tombol manual ditekan.");
    mulaiMemberiMakan("tombol manual");
    // Tunggu tombol dilepas supaya satu tekanan = satu pemberian pakan.
    while (digitalRead(PIN_TOMBOL) == LOW) {
      delay(10);
    }
  }

  // Perintah Serial Monitor, dibaca tanpa menahan loop.
  static String barisPerintah;
  while (Serial.available() > 0) {
    const char karakter = static_cast<char>(Serial.read());
    if (karakter == '\n' || karakter == '\r') {
      if (barisPerintah.length() > 0) {
        // Baris "YYYY-MM-DD HH:MM" = atur jam RTC; selain itu = perintah.
        if (barisPerintah.length() == 16 && barisPerintah.charAt(4) == '-') {
          aturJamRtc(barisPerintah);
        } else {
          tanganiPerintahSerial(barisPerintah.charAt(0));
        }
        barisPerintah = "";
      }
    } else {
      barisPerintah += karakter;
    }
  }
}
