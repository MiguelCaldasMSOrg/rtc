/*
 * DS3231 + AT24C32 (ZS-042 / "MH" module) - full console for Arduino
 * Library-free: only Wire.h, direct register access.
 *
 * Wiring (BBC micro:bit V2): VCC->3V  GND->GND  SDA->P20  SCL->P19
 *                            SQW/INT->P2 (needed for alarm interrupts)
 *                            32K not connected
 * Wire by the module's silkscreen labels; ZS-042 clone layouts vary.
 *
 * Serial monitor: 9600 baud, line ending = Newline
 *
 * TIME
 *   T YYYY-MM-DD HH:MM:SS   set date & time (24h)
 *   R                       read time once
 *   C                       continuous read (any char stops)
 *   D                       temperature (deg C)
 *
 * ALARMS  (INT/SQW pulls LOW when an alarm fires; it is open-drain)
 *   A1 HH:MM:SS             alarm 1, daily at that time
 *   A1 DD HH:MM:SS          alarm 1, on that day-of-month
 *   A1 SEC ss               alarm 1, every minute when seconds == ss
 *   A1 EVERY                alarm 1, once per second
 *   A2 HH:MM                alarm 2, daily at that time
 *   A2 DD HH:MM             alarm 2, on that day-of-month
 *   A2 EVERY                alarm 2, once per minute (at :00)
 *   AOFF 1 | AOFF 2         disable an alarm
 *   AS                      show alarm settings + flags
 *   SQ off|1|1024|4096|8192 square-wave output on INT/SQW (off = alarm mode)
 *
 * EEPROM  (AT24C32, 4096 bytes, 32-byte pages, default address 0x57)
 *   EW addr text...         write a string at addr
 *   EB addr b0 b1 b2 ...    write raw bytes (decimal or 0x..)
 *   ER addr len             hex + ASCII dump
 *   EF addr len value       fill a range with one byte value
 *   EEPROM ERASE            erase whole EEPROM to 0xFF
 *
 * MISC
 *   S    I2C bus scan       H    help
 *
 * HARDWARE NOTE: power this project from 3 V, not 5 V. Many ZS-042 clones
 * have pull-ups to VCC and a diode/resistor coin-cell charging path. A CR2032
 * is not rechargeable: inspect the exact board and ensure that it cannot
 * charge the cell. See README.md for wiring, alarm, and battery details.
 */

#include <Wire.h>

/* =======================================================================
 *  USER TYPES - MUST STAY ABOVE EVERY FUNCTION DEFINITION.
 *
 *  The Arduino IDE auto-generates function prototypes and inserts them
 *  just before the first function definition in the file. Any struct or
 *  enum used in a function signature must therefore be declared above
 *  that point, or the generated prototypes fail with
 *  "'Tm' was not declared in this scope". This applies to by-value
 *  parameters (enums) exactly as much as to references.
 * ======================================================================= */
struct Tm {
  uint16_t year;
  uint8_t mon, day, dow, hh, mm, ss;
};

enum A1Mode {
  A1_EVERY_SEC, A1_MATCH_SEC, A1_MATCH_HMS, A1_MATCH_DATE_HMS
};
enum A2Mode {
  A2_EVERY_MIN, A2_MATCH_HM, A2_MATCH_DATE_HM
};

/* ================= configuration ================= */
const uint8_t DS3231_ADDR = 0x68;
const uint8_t EEPROM_ADDR = 0x57;   // change if A0/A1/A2 jumpers are set
const uint8_t SQW_PIN = 2;          // must be an interrupt-capable pin
const uint16_t EEPROM_SIZE = 4096;  // AT24C32 = 32 Kbit = 4096 bytes
const uint8_t EEPROM_PAGE = 32;     // page size in bytes

/* ================= DS3231 registers ================= */
#define REG_TIME 0x00
#define REG_A1 0x07
#define REG_A2 0x0B
#define REG_CONTROL 0x0E
#define REG_STATUS 0x0F
#define REG_TEMP 0x11

// Control register bits
#define CTRL_EOSC 0x80
#define CTRL_BBSQW 0x40
#define CTRL_CONV 0x20
#define CTRL_RS2 0x10
#define CTRL_RS1 0x08
#define CTRL_INTCN 0x04
#define CTRL_A2IE 0x02
#define CTRL_A1IE 0x01

// Status register bits
#define STAT_OSF 0x80
#define STAT_EN32K 0x08
#define STAT_BSY 0x04
#define STAT_A2F 0x02
#define STAT_A1F 0x01

/* ================= BCD helpers ================= */
static uint8_t bcd2dec(uint8_t b) {
  return (b >> 4) * 10 + (b & 0x0F);
}
static uint8_t dec2bcd(uint8_t d) {
  return ((d / 10) << 4) | (d % 10);
}

/* ================= low-level I2C ================= */
bool rtcWrite(uint8_t reg, const uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) {
    Wire.write(buf[i]);
  }
  return Wire.endTransmission() == 0;
}

bool rtcRead(uint8_t reg, uint8_t *buf, uint8_t len) {
  memset(buf, 0, len);
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((int)DS3231_ADDR, (int)len) != len) {
    return false;
  }
  for (uint8_t i = 0; i < len; i++) {
    buf[i] = Wire.read();
  }
  return true;
}

bool rtcSetReg(uint8_t reg, uint8_t v) {
  return rtcWrite(reg, &v, 1);
}

bool rtcUpdateReg(uint8_t reg, uint8_t clearMask, uint8_t setMask) {
  uint8_t v;
  if (!rtcRead(reg, &v, 1)) {
    return false;
  }
  v = (v & ~clearMask) | setMask;
  return rtcSetReg(reg, v);
}

/* ================= day-of-week (Sakamoto), 1 = Sunday ================= */
uint8_t dayOfWeek(uint16_t y, uint8_t m, uint8_t d) {
  static const uint8_t t[] = {
    0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
  };
  if (m < 3) {
    y--;
  }
  return ((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7) + 1;
}

bool isValidDate(int year, int mon, int day) {
  static const uint8_t daysPerMonth[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };
  if (year < 2000 || year > 2099 || mon < 1 || mon > 12 || day < 1) {
    return false;
  }
  uint8_t maxDay = daysPerMonth[mon - 1];
  bool leapYear = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (mon == 2 && leapYear) {
    maxDay++;
  }
  return day <= maxDay;
}

/* ================= time ================= */
bool setTime(uint16_t year, uint8_t mon, uint8_t day, uint8_t hh, uint8_t mm, uint8_t ss) {
  uint8_t b[7];
  b[0] = dec2bcd(ss) & 0x7F;
  b[1] = dec2bcd(mm);
  b[2] = dec2bcd(hh);  // bit6 = 0 -> 24-hour mode
  b[3] = dayOfWeek(year, mon, day);
  b[4] = dec2bcd(day);
  b[5] = dec2bcd(mon);
  b[6] = dec2bcd((uint8_t)(year - 2000));
  if (!rtcWrite(REG_TIME, b, 7)) {
    return false;
  }
  return rtcUpdateReg(REG_STATUS, STAT_OSF, 0);  // clear stop flag
}

bool getTime(Tm &t) {
  uint8_t b[7];
  if (!rtcRead(REG_TIME, b, 7)) {
    return false;
  }
  t.ss = bcd2dec(b[0] & 0x7F);
  t.mm = bcd2dec(b[1] & 0x7F);
  if (b[2] & 0x40) {
    // 12-hour mode fallback
    uint8_t h = bcd2dec(b[2] & 0x1F);
    if (b[2] & 0x20) {
      h = (h % 12) + 12;
    } else {
      h = h % 12;
    }
    t.hh = h;
  } else {
    t.hh = bcd2dec(b[2] & 0x3F);
  }
  t.dow = b[3] & 0x07;
  t.day = bcd2dec(b[4] & 0x3F);
  t.mon = bcd2dec(b[5] & 0x1F);
  t.year = 2000 + bcd2dec(b[6]);
  return true;
}

float getTemperature() {
  uint8_t b[2];
  if (!rtcRead(REG_TEMP, b, 2)) {
    return NAN;
  }
  return (float)(int8_t)b[0] + ((b[1] >> 6) * 0.25f);
}

/* ================= alarms ================= */
/*
 * Alarm 1 mask bits A1M1..A1M4 sit in bit7 of regs 0x07..0x0A.
 * Alarm 2 mask bits A2M2..A2M4 sit in bit7 of regs 0x0B..0x0D.
 * DY/DT (bit6 of the day/date register): 0 = match date, 1 = match weekday.
 */

bool setAlarm1(A1Mode mode, uint8_t day, uint8_t hh, uint8_t mm, uint8_t ss) {
  uint8_t b[4];
  b[0] = dec2bcd(ss);
  b[1] = dec2bcd(mm);
  b[2] = dec2bcd(hh);   // bit6 = 0 -> 24-hour
  b[3] = dec2bcd(day);  // bit6 = 0 -> match date of month

  switch (mode) {
    case A1_EVERY_SEC:
      b[0] |= 0x80;
      b[1] |= 0x80;
      b[2] |= 0x80;
      b[3] |= 0x80;
      break;
    case A1_MATCH_SEC:
      b[1] |= 0x80;
      b[2] |= 0x80;
      b[3] |= 0x80;
      break;
    case A1_MATCH_HMS:
      b[3] |= 0x80;
      break;
    case A1_MATCH_DATE_HMS:
      break;  // no mask bits set
  }
  if (!rtcWrite(REG_A1, b, 4)) {
    return false;
  }

  if (!rtcUpdateReg(REG_CONTROL, 0, CTRL_INTCN | CTRL_A1IE)) {
    return false;
  }
  return rtcUpdateReg(REG_STATUS, STAT_A1F, 0);
}

bool setAlarm2(A2Mode mode, uint8_t day, uint8_t hh, uint8_t mm) {
  uint8_t b[3];
  b[0] = dec2bcd(mm);
  b[1] = dec2bcd(hh);
  b[2] = dec2bcd(day);

  switch (mode) {
    case A2_EVERY_MIN:
      b[0] |= 0x80;
      b[1] |= 0x80;
      b[2] |= 0x80;
      break;
    case A2_MATCH_HM:
      b[2] |= 0x80;
      break;
    case A2_MATCH_DATE_HM:
      break;
  }
  if (!rtcWrite(REG_A2, b, 3)) {
    return false;
  }

  if (!rtcUpdateReg(REG_CONTROL, 0, CTRL_INTCN | CTRL_A2IE)) {
    return false;
  }
  return rtcUpdateReg(REG_STATUS, STAT_A2F, 0);
}

bool disableAlarm(uint8_t n) {
  if (n != 1 && n != 2) {
    return false;
  }
  uint8_t enableBit = (n == 1) ? CTRL_A1IE : CTRL_A2IE;
  uint8_t flagBit = (n == 1) ? STAT_A1F : STAT_A2F;
  if (!rtcUpdateReg(REG_CONTROL, enableBit, 0)) {
    return false;
  }
  return rtcUpdateReg(REG_STATUS, flagBit, 0);
}

// Reports which alarms fired (bit0 = A1, bit1 = A2) and clears their flags.
bool checkAndClearAlarms(uint8_t &fired) {
  uint8_t st;
  if (!rtcRead(REG_STATUS, &st, 1)) {
    return false;
  }
  fired = st & (STAT_A1F | STAT_A2F);
  if (fired) {
    return rtcSetReg(REG_STATUS, st & ~fired);
  }
  return true;
}

// rate: 0 = alarm/interrupt mode, else 1, 1024, 4096 or 8192 Hz square wave
bool setSquareWave(uint16_t rate) {
  uint8_t c;
  if (!rtcRead(REG_CONTROL, &c, 1)) {
    return false;
  }
  c &= ~(CTRL_RS1 | CTRL_RS2);
  switch (rate) {
    case 0:
      c |= CTRL_INTCN;
      return rtcSetReg(REG_CONTROL, c);
    case 1:
      break;  // RS = 00
    case 1024:
      c |= CTRL_RS1;
      break;  // RS = 01
    case 4096:
      c |= CTRL_RS2;
      break;  // RS = 10
    case 8192:
      c |= CTRL_RS1 | CTRL_RS2;
      break;  // RS = 11
    default:
      return false;
  }
  c &= ~CTRL_INTCN;  // square-wave mode
  return rtcSetReg(REG_CONTROL, c);
}

/* ================= AT24C32 EEPROM ================= */
bool eepromWriteByte(uint16_t addr, uint8_t val) {
  if (addr >= EEPROM_SIZE) {
    return false;
  }
  Wire.beginTransmission(EEPROM_ADDR);
  Wire.write((uint8_t)(addr >> 8));
  Wire.write((uint8_t)(addr & 0xFF));
  Wire.write(val);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  delay(6);  // write cycle time (max 5 ms)
  return true;
}

// Handles page boundaries and the Wire buffer limit automatically.
bool eepromWrite(uint16_t addr, const uint8_t *data, uint16_t len) {
  if ((uint32_t)addr + len > EEPROM_SIZE) {
    return false;
  }
  while (len) {
    uint8_t room = EEPROM_PAGE - (addr % EEPROM_PAGE);  // to end of page
    uint8_t chunk = (len < room) ? (uint8_t)len : room;
    if (chunk > 16) {
      chunk = 16;  // stay inside the 32-byte Wire buffer
    }
    Wire.beginTransmission(EEPROM_ADDR);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    for (uint8_t i = 0; i < chunk; i++) {
      Wire.write(data[i]);
    }
    if (Wire.endTransmission() != 0) {
      return false;
    }
    delay(6);
    addr += chunk;
    data += chunk;
    len -= chunk;
  }
  return true;
}

bool eepromRead(uint16_t addr, uint8_t *data, uint16_t len) {
  if ((uint32_t)addr + len > EEPROM_SIZE) {
    return false;
  }
  while (len) {
    uint8_t chunk = (len < 16) ? (uint8_t)len : 16;
    Wire.beginTransmission(EEPROM_ADDR);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    if (Wire.endTransmission(false) != 0) {
      return false;
    }
    if (Wire.requestFrom((int)EEPROM_ADDR, (int)chunk) != chunk) {
      return false;
    }
    for (uint8_t i = 0; i < chunk; i++) {
      data[i] = Wire.read();
    }
    addr += chunk;
    data += chunk;
    len -= chunk;
  }
  return true;
}

bool eepromFill(uint16_t addr, uint16_t len, uint8_t val) {
  if ((uint32_t)addr + len > EEPROM_SIZE) {
    return false;
  }
  uint8_t buf[16];
  memset(buf, val, sizeof(buf));
  while (len) {
    uint16_t chunk = (len < sizeof(buf)) ? len : sizeof(buf);
    if (!eepromWrite(addr, buf, chunk)) {
      return false;
    }
    addr += chunk;
    len -= chunk;
  }
  return true;
}

/* ================= printing ================= */
void print2(uint8_t v) {
  if (v < 10) {
    Serial.print('0');
  }
  Serial.print(v);
}

void printTime() {
  Tm t;
  if (!getTime(t)) {
    Serial.println(F("ERROR: no answer from RTC at 0x68"));
    return;
  }
  static const char *dows[] = {
    "---", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
  };
  Serial.print(t.year);
  Serial.print('-');
  print2(t.mon);
  Serial.print('-');
  print2(t.day);
  Serial.print(' ');
  print2(t.hh);
  Serial.print(':');
  print2(t.mm);
  Serial.print(':');
  print2(t.ss);
  Serial.print(F("  "));
  Serial.println(dows[t.dow <= 7 ? t.dow : 0]);
}

void printAlarmStatus() {
  uint8_t c, st;
  uint8_t a1[4], a2[3];
  if (!rtcRead(REG_CONTROL, &c, 1) || !rtcRead(REG_STATUS, &st, 1) || !rtcRead(REG_A1, a1, 4) || !rtcRead(REG_A2, a2, 3)) {
    Serial.println(F("Alarm status read failed."));
    return;
  }

  Serial.print(F("CONTROL=0x"));
  Serial.print(c, HEX);
  Serial.print(F("  STATUS=0x"));
  Serial.println(st, HEX);
  Serial.print(F("  mode: "));
  Serial.println((c & CTRL_INTCN) ? F("alarm/interrupt") : F("square wave"));

  Serial.print(F("  A1 "));
  Serial.print((c & CTRL_A1IE) ? F("ENABLED ") : F("disabled"));
  Serial.print(F("  date="));
  Serial.print(bcd2dec(a1[3] & 0x3F));
  Serial.print(F(" "));
  print2(bcd2dec(a1[2] & 0x3F));
  Serial.print(F(":"));
  print2(bcd2dec(a1[1] & 0x7F));
  Serial.print(F(":"));
  print2(bcd2dec(a1[0] & 0x7F));
  Serial.print(F("  mask="));
  Serial.print((a1[3] >> 7) & 1);
  Serial.print((a1[2] >> 7) & 1);
  Serial.print((a1[1] >> 7) & 1);
  Serial.print((a1[0] >> 7) & 1);
  Serial.print(F("  flag="));
  Serial.println((st & STAT_A1F) ? 1 : 0);

  Serial.print(F("  A2 "));
  Serial.print((c & CTRL_A2IE) ? F("ENABLED ") : F("disabled"));
  Serial.print(F("  date="));
  Serial.print(bcd2dec(a2[2] & 0x3F));
  Serial.print(F(" "));
  print2(bcd2dec(a2[1] & 0x3F));
  Serial.print(F(":"));
  print2(bcd2dec(a2[0] & 0x7F));
  Serial.print(F("  mask="));
  Serial.print((a2[2] >> 7) & 1);
  Serial.print((a2[1] >> 7) & 1);
  Serial.print((a2[0] >> 7) & 1);
  Serial.print(F("  flag="));
  Serial.println((st & STAT_A2F) ? 1 : 0);
}

void eepromDump(uint16_t addr, uint16_t len) {
  uint8_t buf[16];
  while (len) {
    uint16_t chunk = (len < 16) ? len : 16;
    if (!eepromRead(addr, buf, chunk)) {
      Serial.println(F("EEPROM read failed."));
      return;
    }
    if (addr < 0x1000) {
      Serial.print('0');
    }
    if (addr < 0x0100) {
      Serial.print('0');
    }
    if (addr < 0x0010) {
      Serial.print('0');
    }
    Serial.print(addr, HEX);
    Serial.print(F("  "));
    for (uint16_t i = 0; i < chunk; i++) {
      if (buf[i] < 0x10) {
        Serial.print('0');
      }
      Serial.print(buf[i], HEX);
      Serial.print(' ');
    }
    for (uint16_t i = chunk; i < 16; i++) {
      Serial.print(F("   "));
    }
    Serial.print(F(" |"));
    for (uint16_t i = 0; i < chunk; i++) {
      Serial.print((buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.');
    }
    Serial.println('|');
    addr += chunk;
    len -= chunk;
  }
}

void scanBus() {
  Serial.println(F("Scanning I2C..."));
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  found 0x"));
      Serial.print(a, HEX);
      if (a == DS3231_ADDR) {
        Serial.print(F("  (DS3231 RTC)"));
      }
      if (a == EEPROM_ADDR) {
        Serial.print(F("  (AT24C32 EEPROM)"));
      }
      Serial.println();
    }
  }
  Serial.println(F("Scan done."));
}

void help() {
  Serial.println(F("\n-- time --"));
  Serial.println(F("  T YYYY-MM-DD HH:MM:SS   set clock"));
  Serial.println(F("  R / C / D               read once / continuous / temperature"));
  Serial.println(F("-- alarms --"));
  Serial.println(F("  A1 HH:MM:SS             daily"));
  Serial.println(F("  A1 DD HH:MM:SS          monthly on day DD"));
  Serial.println(F("  A1 SEC ss               every minute at second ss"));
  Serial.println(F("  A1 EVERY                once per second"));
  Serial.println(F("  A2 HH:MM | A2 DD HH:MM | A2 EVERY"));
  Serial.println(F("  AOFF 1|2                disable alarm"));
  Serial.println(F("  AS                      show alarm settings"));
  Serial.println(F("  SQ off|1|1024|4096|8192 square-wave output"));
  Serial.println(F("-- eeprom (AT24C32, 0..4095) --"));
  Serial.println(F("  EW addr text...         write string"));
  Serial.println(F("  EB addr b0 b1 ...       write bytes"));
  Serial.println(F("  ER addr len             hex dump"));
  Serial.println(F("  EF addr len value       fill"));
  Serial.println(F("  EEPROM ERASE            erase all to 0xFF"));
  Serial.println(F("-- misc --  S scan   H help"));
}

/* ================= alarm interrupt ================= */
volatile bool alarmFlag = false;
void onAlarmISR() {
  alarmFlag = true;
}

/* ================= command handling ================= */
// Skips leading spaces, returns pointer to next non-space char.
const char *skipSpaces(const char *p) {
  while (*p == ' ') {
    p++;
  }
  return p;
}

bool parseLongToken(const char *&p, long &value) {
  p = skipSpaces(p);
  char *end;
  value = strtol(p, &end, 0);
  if (end == p || (*end && *end != ' ')) {
    return false;
  }
  p = end;
  return true;
}

void cmdSetTime(const char *arg) {
  int Y, Mo, D, h, m, s;
  if (sscanf(arg, " %d-%d-%d %d:%d:%d", &Y, &Mo, &D, &h, &m, &s) != 6) {
    Serial.println(F("Use: T YYYY-MM-DD HH:MM:SS"));
    return;
  }
  if (!isValidDate(Y, Mo, D) || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) {
    Serial.println(F("Out of range."));
    return;
  }
  if (setTime(Y, Mo, D, h, m, s)) {
    Serial.print(F("Set -> "));
    printTime();
  } else {
    Serial.println(F("Write failed."));
  }
}

void cmdAlarm1(const char *arg) {
  int a, b, c, d;
  arg = skipSpaces(arg);
  if (strncasecmp(arg, "EVERY", 5) == 0) {
    if (!setAlarm1(A1_EVERY_SEC, 1, 0, 0, 0)) {
      Serial.println(F("A1 write failed."));
      return;
    }
    alarmFlag = true;
    Serial.println(F("A1: fires once per second."));
    return;
  }
  if (strncasecmp(arg, "SEC", 3) == 0) {
    if (sscanf(arg + 3, " %d", &a) != 1 || a < 0 || a > 59) {
      Serial.println(F("Use: A1 SEC ss"));
      return;
    }
    if (!setAlarm1(A1_MATCH_SEC, 1, 0, 0, (uint8_t)a)) {
      Serial.println(F("A1 write failed."));
      return;
    }
    alarmFlag = true;
    Serial.print(F("A1: every minute at second "));
    Serial.println(a);
    return;
  }
  if (sscanf(arg, "%d %d:%d:%d", &a, &b, &c, &d) == 4) {
    // DD HH:MM:SS
    if (a < 1 || a > 31 || b < 0 || b > 23 || c < 0 || c > 59 || d < 0 || d > 59) {
      Serial.println(F("Out of range."));
      return;
    }
    if (!setAlarm1(A1_MATCH_DATE_HMS, a, b, c, d)) {
      Serial.println(F("A1 write failed."));
      return;
    }
    alarmFlag = true;
    Serial.print(F("A1: day "));
    Serial.print(a);
    Serial.print(F(" at "));
    print2(b);
    Serial.print(':');
    print2(c);
    Serial.print(':');
    print2(d);
    Serial.println();
    return;
  }
  if (sscanf(arg, "%d:%d:%d", &b, &c, &d) == 3) {
    // HH:MM:SS
    if (b < 0 || b > 23 || c < 0 || c > 59 || d < 0 || d > 59) {
      Serial.println(F("Out of range."));
      return;
    }
    if (!setAlarm1(A1_MATCH_HMS, 1, b, c, d)) {
      Serial.println(F("A1 write failed."));
      return;
    }
    alarmFlag = true;
    Serial.print(F("A1: daily at "));
    print2(b);
    Serial.print(':');
    print2(c);
    Serial.print(':');
    print2(d);
    Serial.println();
    return;
  }
  Serial.println(F("Use: A1 HH:MM:SS | A1 DD HH:MM:SS | A1 SEC ss | A1 EVERY"));
}

void cmdAlarm2(const char *arg) {
  int a, b, c;
  arg = skipSpaces(arg);
  if (strncasecmp(arg, "EVERY", 5) == 0) {
    if (!setAlarm2(A2_EVERY_MIN, 1, 0, 0)) {
      Serial.println(F("A2 write failed."));
      return;
    }
    alarmFlag = true;
    Serial.println(F("A2: fires once per minute at :00."));
    return;
  }
  if (sscanf(arg, "%d %d:%d", &a, &b, &c) == 3) {
    // DD HH:MM
    if (a < 1 || a > 31 || b < 0 || b > 23 || c < 0 || c > 59) {
      Serial.println(F("Out of range."));
      return;
    }
    if (!setAlarm2(A2_MATCH_DATE_HM, a, b, c)) {
      Serial.println(F("A2 write failed."));
      return;
    }
    alarmFlag = true;
    Serial.print(F("A2: day "));
    Serial.print(a);
    Serial.print(F(" at "));
    print2(b);
    Serial.print(':');
    print2(c);
    Serial.println();
    return;
  }
  if (sscanf(arg, "%d:%d", &b, &c) == 2) {
    // HH:MM
    if (b < 0 || b > 23 || c < 0 || c > 59) {
      Serial.println(F("Out of range."));
      return;
    }
    if (!setAlarm2(A2_MATCH_HM, 1, b, c)) {
      Serial.println(F("A2 write failed."));
      return;
    }
    alarmFlag = true;
    Serial.print(F("A2: daily at "));
    print2(b);
    Serial.print(':');
    print2(c);
    Serial.println();
    return;
  }
  Serial.println(F("Use: A2 HH:MM | A2 DD HH:MM | A2 EVERY"));
}

void cmdSquareWave(const char *arg) {
  arg = skipSpaces(arg);
  if (strncasecmp(arg, "OFF", 3) == 0) {
    if (!setSquareWave(0)) {
      Serial.println(F("Control write failed."));
      return;
    }
    alarmFlag = true;
    Serial.println(F("INT/SQW back to alarm mode."));
    return;
  }
  long r = atol(arg);
  if (r != 1 && r != 1024 && r != 4096 && r != 8192) {
    Serial.println(F("Use: SQ off|1|1024|4096|8192"));
    return;
  }
  if (!setSquareWave((uint16_t)r)) {
    Serial.println(F("Control write failed."));
    return;
  }
  Serial.print(F("Square wave: "));
  Serial.print(r);
  Serial.println(F(" Hz"));
  Serial.println(F("(alarms cannot drive the pin while this is active)"));
}

void cmdEepromWriteString(const char *arg) {
  arg = skipSpaces(arg);
  char *end;
  long addr = strtol(arg, &end, 0);
  if (end == arg || addr < 0 || addr >= EEPROM_SIZE) {
    Serial.println(F("Use: EW addr text..."));
    return;
  }
  const char *text = skipSpaces(end);
  uint16_t len = strlen(text) + 1;  // include terminating NUL
  if ((uint32_t)addr + len > EEPROM_SIZE) {
    Serial.println(F("String and terminator exceed EEPROM bounds."));
    return;
  }
  if (eepromWrite((uint16_t)addr, (const uint8_t *)text, len)) {
    Serial.print(F("Wrote "));
    Serial.print(len);
    Serial.print(F(" bytes at "));
    Serial.println(addr);
  } else {
    Serial.println(F("EEPROM write failed."));
  }
}

void cmdEepromWriteBytes(const char *arg) {
  arg = skipSpaces(arg);
  char *end;
  long addr = strtol(arg, &end, 0);
  if (end == arg || addr < 0 || addr >= EEPROM_SIZE) {
    Serial.println(F("Use: EB addr b0 b1 ..."));
    return;
  }
  uint8_t buf[24];
  uint8_t n = 0;
  const char *p = end;
  while (true) {
    p = skipSpaces(p);
    if (!*p) {
      break;
    }
    if (n >= sizeof(buf)) {
      Serial.println(F("Too many byte values; maximum is 24."));
      return;
    }
    long v = strtol(p, &end, 0);
    if (end == p) {
      Serial.println(F("Invalid byte value."));
      return;
    }
    if (v < 0 || v > 255) {
      Serial.println(F("Byte values must be between 0 and 255."));
      return;
    }
    buf[n++] = (uint8_t)v;
    p = end;
  }
  if (!n) {
    Serial.println(F("No byte values given."));
    return;
  }
  if (eepromWrite((uint16_t)addr, buf, n)) {
    Serial.print(F("Wrote "));
    Serial.print(n);
    Serial.print(F(" bytes at "));
    Serial.println(addr);
  } else {
    Serial.println(F("EEPROM write failed."));
  }
}

void cmdEepromRead(const char *arg) {
  long addr, len;
  if (!parseLongToken(arg, addr) || !parseLongToken(arg, len) || *skipSpaces(arg) || addr < 0 || len <= 0 || addr >= EEPROM_SIZE || len > EEPROM_SIZE - addr) {
    Serial.println(F("Use: ER addr len   (addr+len <= 4096)"));
    return;
  }
  eepromDump((uint16_t)addr, (uint16_t)len);
}

void cmdEepromFill(const char *arg) {
  long addr, len, val;
  if (!parseLongToken(arg, addr) || !parseLongToken(arg, len) || !parseLongToken(arg, val) || *skipSpaces(arg) || addr < 0 || len <= 0 || addr >= EEPROM_SIZE || len > EEPROM_SIZE - addr || val < 0 || val > 255) {
    Serial.println(F("Use: EF addr len value"));
    return;
  }
  Serial.print(F("Filling..."));
  Serial.println(eepromFill((uint16_t)addr, (uint16_t)len, (uint8_t)val) ? F(" done.") : F(" failed."));
}

void continuousRead() {
  Serial.println(F("Continuous mode - send any character to stop."));
  while (!Serial.available()) {
    printTime();
    delay(1000);
  }
  while (Serial.available()) {
    Serial.read();
  }
  Serial.println(F("Stopped."));
}

/* ================= main ================= */
void setup() {
  Serial.begin(9600);
  while (!Serial) {
    ;
  }
  Wire.begin();

  pinMode(SQW_PIN, INPUT_PULLUP);  // INT/SQW is open-drain
  attachInterrupt(digitalPinToInterrupt(SQW_PIN), onAlarmISR, FALLING);
  Serial.println(F("\nDS3231 + AT24C32 console"));
  scanBus();

  uint8_t status;
  if (!rtcRead(REG_STATUS, &status, 1)) {
    Serial.println(F("WARNING: RTC status read failed."));
  } else {
    if (status & (STAT_A1F | STAT_A2F)) {
      alarmFlag = true;
    }
    if (status & STAT_OSF) {
      Serial.println(F("WARNING: oscillator stop flag set - time invalid, set it with T."));
    }
  }

  printTime();
  help();
}

void loop() {
  /* --- report alarms --- */
  if (alarmFlag) {
    alarmFlag = false;
    uint8_t fired;
    if (!checkAndClearAlarms(fired)) {
      Serial.println(F("Alarm status read/clear failed; will retry."));
      alarmFlag = true;
      delay(10);
    } else if (fired) {
      Serial.print(F("*** ALARM"));
      if (fired & STAT_A1F) {
        Serial.print(F(" 1"));
      }
      if (fired & STAT_A2F) {
        Serial.print(F(" 2"));
      }
      Serial.print(F(" at "));
      printTime();
    }
  }

  /* --- console --- */
  static char line[72];
  static uint8_t n = 0;
  static bool lineOverflow = false;

  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') {
      continue;
    }
    if (ch != '\n') {
      if (n < sizeof(line) - 1) {
        line[n++] = ch;
      } else {
        lineOverflow = true;
      }
      continue;
    }

    if (lineOverflow) {
      n = 0;
      lineOverflow = false;
      Serial.println(F("Command too long; maximum is 71 characters."));
      continue;
    }
    line[n] = '\0';
    n = 0;
    const char *p = skipSpaces(line);
    if (!*p) {
      continue;
    }

    if (strncasecmp(p, "AOFF", 4) == 0) {
      int which = atoi(skipSpaces(p + 4));
      if (which == 1 || which == 2) {
        if (!disableAlarm((uint8_t)which)) {
          Serial.println(F("Alarm disable failed."));
          continue;
        }
        alarmFlag = true;
        Serial.print(F("Alarm "));
        Serial.print(which);
        Serial.println(F(" disabled."));
      } else {
        Serial.println(F("Use: AOFF 1 | AOFF 2"));
      }
    } else if (strncasecmp(p, "AS", 2) == 0) {
      printAlarmStatus();
    } else if (strncasecmp(p, "A1", 2) == 0) {
      cmdAlarm1(p + 2);
    } else if (strncasecmp(p, "A2", 2) == 0) {
      cmdAlarm2(p + 2);
    } else if (strncasecmp(p, "SQ", 2) == 0) {
      cmdSquareWave(p + 2);
    } else if (strncasecmp(p, "EW", 2) == 0) {
      cmdEepromWriteString(p + 2);
    } else if (strncasecmp(p, "EB", 2) == 0) {
      cmdEepromWriteBytes(p + 2);
    } else if (strncasecmp(p, "ER", 2) == 0) {
      cmdEepromRead(p + 2);
    } else if (strncasecmp(p, "EF", 2) == 0) {
      cmdEepromFill(p + 2);
    } else if (strcasecmp(p, "EEPROM ERASE") == 0) {
      Serial.print(F("Erasing 4096 bytes..."));
      Serial.println(eepromFill(0, EEPROM_SIZE, 0xFF) ? F(" done.") : F(" failed."));
    } else {
      switch (toupper(p[0])) {
        case 'T':
          cmdSetTime(p + 1);
          break;
        case 'R':
          printTime();
          break;
        case 'C':
          continuousRead();
          break;
        case 'D':
          Serial.print(F("Temp: "));
          Serial.print(getTemperature(), 2);
          Serial.println(F(" C"));
          break;
        case 'S':
          scanBus();
          break;
        case 'H':
          help();
          break;
        default:
          Serial.println(F("Unknown command. H for help."));
      }
    }
  }
}
