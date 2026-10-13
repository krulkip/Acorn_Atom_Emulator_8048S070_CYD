// V140b physical Atom keyboard via MCP23017 on GPIO19/20.
// Uses software I2C intentionally: no <Wire.h>, so the proven V139 display
// initialization/memory environment is left unchanged.
#include <Arduino.h>
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "atom_hw_kbd.h"

static const uint8_t SDA_PIN = 19;
static const uint8_t SCL_PIN = 20;
static uint8_t MCP_ADDR = 0x20;
// V191: 0x20 direct 8x8; 0x21 legacy Hackjunk; 0x22 small auxiliary.
static uint8_t kbd_type = 0; // 1=FULL, 2=HACKJUNK, 3=SMALL
static const uint8_t GPIOB = 0x13;
static uint8_t full_previous[8][8] = {};
static uint8_t small_aux_high=1;
static const uint8_t full_to_atom[8][8] = {
  {5, 13, 7, 1, 56, 50, 44, 38},
  {32, 26, 19, 2, 20, 30, 0xFF, 0xFF},
  {18, 31, 4, 29, 15, 59, 47, 17},
  {41, 52, 16, 10, 45, 42, 25, 0xFF},
  {12, 0xFF, 39, 53, 21, 9, 3, 58},
  {46, 40, 34, 14, 48, 36, 37, 0xFF},
  {24, 0xFF, 11, 23, 27, 35, 33, 54},
  {22, 28, 8, 57, 51, 0xFF, 0xFF, 0xFF}
};

static const uint8_t IODIRA = 0x00;
static const uint8_t IODIRB = 0x01;
static const uint8_t IOCON  = 0x0A;
static const uint8_t GPPUA  = 0x0C;
static const uint8_t GPPUB  = 0x0D;
static const uint8_t GPIOA  = 0x12;
static const uint8_t OLATA  = 0x14;

static const uint8_t ATOM_PA_TO_GPA[4] = {2,1,0,3};
static uint8_t cached_rows[10] = {0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F};
static uint8_t cached_ctrl_high=1, cached_shift_high=1, cached_rept_high=1;
static uint8_t cached_break_high=1, break_last_raw_high=1;
static uint32_t break_changed_ms=0;
static uint8_t hw_present=0;
static uint32_t next_scan_us=0;

// V190: external FE71 is a discrete-key interface.  A real physical tap can
// begin and end between two FE71 calls (Frogger was measured at ~0.57 s
// between calls), so retain one recent key-down event.  Normal matrix state
// remains completely unchanged; this latch is only consumed by external FE71.
static volatile uint8_t fe71_event_valid=0;
static volatile uint8_t fe71_event_row=0, fe71_event_col=0;
static volatile uint8_t fe71_event_shift=0, fe71_event_ctrl=0;
static volatile uint32_t fe71_event_us=0;
static const uint32_t FE71_EVENT_MAX_US = 750000u;

static inline void i2cDelay(){ delayMicroseconds(4); }

// V188 frozen from V187: preserve V181's electrical behaviour exactly: LOW means actively
// drive the line low; RELEASE means make the pin high-impedance and let the
// pull-up raise it.  The only change is avoiding the very expensive Arduino
// pinMode() call for every I2C bit.  GPIO19/20 are both in the low GPIO bank.
static inline void gpioDriveLow(uint8_t pin){
  REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << pin));
  REG_WRITE(GPIO_ENABLE_W1TS_REG, (1UL << pin));
}
static inline void gpioRelease(uint8_t pin){
  REG_WRITE(GPIO_ENABLE_W1TC_REG, (1UL << pin));
}
static inline void sdaLow(){ gpioDriveLow(SDA_PIN); }
static inline void sdaRelease(){ gpioRelease(SDA_PIN); }
static inline void sclLow(){ gpioDriveLow(SCL_PIN); }
static inline void sclRelease(){ gpioRelease(SCL_PIN); }
static bool waitSclHigh(){
  sclRelease();
  uint32_t t=micros();
  while(!digitalRead(SCL_PIN)) if((uint32_t)(micros()-t)>1000u) return false;
  return true;
}
static bool i2cStart(){
  sdaRelease(); if(!waitSclHigh()) return false; i2cDelay();
  if(!digitalRead(SDA_PIN)) return false;
  sdaLow(); i2cDelay(); sclLow(); return true;
}
static void i2cStop(){
  sdaLow(); i2cDelay(); waitSclHigh(); i2cDelay(); sdaRelease(); i2cDelay();
}
static bool i2cWriteByte(uint8_t v){
  for(uint8_t i=0;i<8;i++){
    if(v&0x80) sdaRelease(); else sdaLow();
    i2cDelay(); if(!waitSclHigh()) return false; i2cDelay(); sclLow(); v<<=1;
  }
  sdaRelease(); i2cDelay(); if(!waitSclHigh()) return false; i2cDelay();
  bool ack=!digitalRead(SDA_PIN); sclLow(); return ack;
}
static uint8_t i2cReadByte(bool ack){
  uint8_t v=0; sdaRelease();
  for(uint8_t i=0;i<8;i++){
    v<<=1; i2cDelay(); waitSclHigh(); i2cDelay(); if(digitalRead(SDA_PIN)) v|=1; sclLow();
  }
  if(ack) sdaLow(); else sdaRelease();
  i2cDelay(); waitSclHigh(); i2cDelay(); sclLow(); sdaRelease();
  return v;
}
static bool mcpWrite(uint8_t reg,uint8_t value){
  if(!i2cStart()) return false;
  bool ok=i2cWriteByte((MCP_ADDR<<1)|0) && i2cWriteByte(reg) && i2cWriteByte(value);
  i2cStop(); return ok;
}
static bool mcpRead2(uint8_t reg,uint8_t &a,uint8_t &b){
  if(!i2cStart()) return false;
  if(!i2cWriteByte((MCP_ADDR<<1)|0) || !i2cWriteByte(reg)){ i2cStop(); return false; }
  if(!i2cStart()){ i2cStop(); return false; }
  if(!i2cWriteByte((MCP_ADDR<<1)|1)){ i2cStop(); return false; }
  a=i2cReadByte(true); b=i2cReadByte(false); i2cStop(); return true;
}
static bool mcpReadOne(uint8_t reg,uint8_t &value){
  if(!i2cStart()) return false;
  if(!i2cWriteByte((MCP_ADDR<<1)|0) || !i2cWriteByte(reg)){ i2cStop(); return false; }
  if(!i2cStart()){ i2cStop(); return false; }
  if(!i2cWriteByte((MCP_ADDR<<1)|1)){ i2cStop(); return false; }
  value=i2cReadByte(false); i2cStop(); return true;
}
static bool mcpPresent(){
  if(!i2cStart()) return false; bool ok=i2cWriteByte((MCP_ADDR<<1)|0); i2cStop(); return ok;
}
static uint8_t makeGPA(uint8_t atomPA){
  uint8_t out=0xF0;
  for(uint8_t bit=0;bit<4;bit++){
    uint8_t gpa=ATOM_PA_TO_GPA[bit];
    if(atomPA&(1u<<bit)) out|=(1u<<gpa); else out&=~(1u<<gpa);
  }
  return out;
}
static bool selectRow(uint8_t row){ return mcpWrite(OLATA,makeGPA(row&0x0F)); }

extern "C" int atom_hw_kbd_init(void){
  // Establish the same released INPUT_PULLUP state used by V181 once.
  // Thereafter the fast helpers only toggle the output-enable bit.
  pinMode(SDA_PIN,INPUT_PULLUP);
  pinMode(SCL_PIN,INPUT_PULLUP);
  digitalWrite(SDA_PIN,LOW);
  digitalWrite(SCL_PIN,LOW);
  sdaRelease();
  sclRelease();
  delayMicroseconds(20);

  kbd_type=0;
  for(uint8_t addr=0x20;addr<=0x22;addr++){
    MCP_ADDR=addr;
    if(mcpPresent()){ kbd_type=(uint8_t)(addr-0x1F); break; }
  }
  if(!kbd_type){
    Serial.println("[HW-KBD] No keyboard at 0x20/0x21/0x22 on SDA=19 SCL=20");
    hw_present=0; return 0;
  }

  if(!mcpWrite(IOCON,0x00)){ Serial.println("[HW-KBD] IOCON write failed"); hw_present=0; return 0; }
  if(!mcpWrite(IODIRA,kbd_type==1 ? 0x00 : (kbd_type==3 ? 0xFD : 0xF0))){ Serial.println("[HW-KBD] IODIRA write failed"); hw_present=0; return 0; }
  if(!mcpWrite(IODIRB,0xFF)){ Serial.println("[HW-KBD] IODIRB write failed"); hw_present=0; return 0; }
  if(!mcpWrite(GPPUA,kbd_type==1 ? 0x00 : (kbd_type==3 ? 0xFD : 0xF0))){ Serial.println("[HW-KBD] GPPUA write failed"); hw_present=0; return 0; }
  if(!mcpWrite(GPPUB,0xFF)){ Serial.println("[HW-KBD] GPPUB write failed"); hw_present=0; return 0; }
  if(!mcpWrite(OLATA, kbd_type==1 ? 0xFF : (kbd_type==3 ? 0xFF : makeGPA(15)))){ Serial.println("[HW-KBD] row deselect failed"); hw_present=0; return 0; }

  hw_present=1; break_last_raw_high=1; cached_break_high=1; break_changed_ms=millis();
  Serial.printf("[HW-KBD] V191 %s at 0x%02X, SDA=19 SCL=20 (V188 fast software I2C)\n",kbd_type==1?"FULL 8x8":(kbd_type==2?"HACKJUNK":"SMALL"),MCP_ADDR);
  return 1;
}

extern "C" void atom_hw_kbd_scan(void){
  if(!hw_present) return;
  // Limit bus traffic to roughly 500 complete scans/s.  V140e calls this
  // from a dedicated low-priority core-0 task so V139's Arduino loop timing
  // remains untouched.
  uint32_t nowus=micros();
  if((int32_t)(nowus-next_scan_us)<0) return;
  next_scan_us=nowus+2000u;

  if(kbd_type==1){
    // Direct 8x8. Translate physical key legends to the original Atom 10x6 matrix.
    uint8_t rows[10]; for(uint8_t i=0;i<10;i++) rows[i]=0x3F;
    uint8_t ctrl=0,shift=0,rept=0,brk=0;
    for(uint8_t r=0;r<8;r++){
      if(!mcpWrite(OLATA,(uint8_t)~(1u<<r))){hw_present=0;return;}
      delayMicroseconds(20);
      uint8_t b; if(!mcpReadOne(GPIOB,b)){hw_present=0;return;}
      for(uint8_t c=0;c<8;c++){
        uint8_t down=!(b&(1u<<c));
        if(r==1 && c==6) brk=down;
        if(r==4 && c==1) ctrl=down;
        if(r==6 && c==1) shift|=down;
        if(r==7 && c==5) shift|=down;
        if(r==7 && c==6) rept=down;
        uint8_t k=full_to_atom[r][c];
        if(k!=0xFF && down){ rows[k/6]&=(uint8_t)~(1u<<(k%6));
          if(!full_previous[r][c] && !fe71_event_valid){
            fe71_event_row=k/6;fe71_event_col=k%6;
            fe71_event_shift=shift;fe71_event_ctrl=ctrl;
            fe71_event_us=micros();fe71_event_valid=1;
          }
        }
        full_previous[r][c]=down;
      }
    }
    mcpWrite(OLATA,0xFF);
    for(uint8_t i=0;i<10;i++) cached_rows[i]=rows[i];
    cached_ctrl_high=!ctrl;cached_shift_high=!shift;cached_rept_high=!rept;
    uint8_t raw=!brk;uint32_t now=millis();
    if(raw!=break_last_raw_high){break_last_raw_high=raw;break_changed_ms=now;}
    if((uint32_t)(now-break_changed_ms)>=20u) cached_break_high=raw;
    return;
  }
  if(kbd_type==3){
    // Small 0x22 board: direct modifier inputs; AUX1 is an extra R1/C7 switch.
    uint8_t a,b;
    if(!mcpRead2(GPIOA,a,b)){hw_present=0;return;}
    cached_ctrl_high=(b&(1u<<5))?1:0;
    cached_shift_high=(b&(1u<<6))?1:0;
    cached_rept_high=(b&(1u<<7))?1:0;
    uint8_t raw=(a&(1u<<5))?1:0;uint32_t now=millis();
    if(raw!=break_last_raw_high){break_last_raw_high=raw;break_changed_ms=now;}
    if((uint32_t)(now-break_changed_ms)>=20u) cached_break_high=raw;
    // AUX1 is not an Atom character key.  Do not inject it into FE71.
    mcpWrite(OLATA,0xFD);delayMicroseconds(20);
    uint8_t auxb=0xFF;
    if(mcpReadOne(GPIOB,auxb)) small_aux_high=((b&(1u<<7))==0 || (auxb&(1u<<7)))?1:0;
    mcpWrite(OLATA,0xFF);
    return;
  }
  uint8_t lastA=0xFF,lastB=0xFF;
  for(uint8_t row=0;row<10;row++){
    if(!selectRow(row)){ hw_present=0; return; }
    delayMicroseconds(20);
    uint8_t a,b; if(!mcpRead2(GPIOA,a,b)){ hw_present=0; return; }
    lastA=a; lastB=b;
    uint8_t low6=0;
    if(a&(1u<<4)) low6|=1u;
    for(uint8_t logical=1;logical<=5;logical++) if(b&(1u<<(logical-1))) low6|=(1u<<logical);
    // Capture the first new physical key-down edge if no unconsumed FE71
    // event is waiting.  Do not alter cached_rows or the normal held-key path.
    uint8_t old6 = cached_rows[row];
    uint8_t newly_down = (uint8_t)(old6 & (uint8_t)~low6 & 0x3Fu);
    if(newly_down && !fe71_event_valid){
      uint8_t col=0;
      while(col<6 && !(newly_down & (1u<<col))) col++;
      if(col<6){
        fe71_event_row=row; fe71_event_col=col;
        fe71_event_ctrl =(b&(1u<<5))?0u:1u;
        fe71_event_shift=(b&(1u<<6))?0u:1u;
        fe71_event_us=micros();
        fe71_event_valid=1;
      }
    }
    cached_rows[row]=low6;
  }
  cached_ctrl_high =(lastB&(1u<<5))?1:0;
  cached_shift_high=(lastB&(1u<<6))?1:0;
  cached_rept_high =(lastB&(1u<<7))?1:0;
  uint8_t raw=(lastA&(1u<<5))?1:0;
  uint32_t now=millis();
  if(raw!=break_last_raw_high){ break_last_raw_high=raw; break_changed_ms=now; }
  if((uint32_t)(now-break_changed_ms)>=20u) cached_break_high=raw;
  selectRow(15);

}
extern "C" uint8_t atom_hw_kbd_get_port_b(uint8_t row){
  if(!hw_present) return 0xFF;
  uint8_t v=(row<10)?(cached_rows[row]&0x3F):0x3F;
  if(cached_ctrl_high) v|=0x40;
  if(cached_shift_high) v|=0x80;
  return v;
}
extern "C" uint8_t atom_hw_kbd_rept_pressed(void){ return (hw_present&&!cached_rept_high)?1:0; }
extern "C" uint8_t atom_hw_kbd_break_pressed(void){ return (hw_present&&!cached_break_high)?1:0; }

extern "C" uint8_t atom_hw_kbd_take_fe71_event(uint8_t *row,uint8_t *col,uint8_t *shift,uint8_t *ctrl){
  if(!fe71_event_valid) return 0;
  uint32_t age=(uint32_t)(micros()-fe71_event_us);
  if(age>FE71_EVENT_MAX_US){ fe71_event_valid=0; return 0; }
  if(row) *row=fe71_event_row; if(col) *col=fe71_event_col;
  if(shift) *shift=fe71_event_shift; if(ctrl) *ctrl=fe71_event_ctrl;
  fe71_event_valid=0;
  return 1;
}
extern "C" void atom_hw_kbd_clear_fe71_event(void){ fe71_event_valid=0; }
