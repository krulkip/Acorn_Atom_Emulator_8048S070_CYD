// CYD V191 - automatic support for all three MCP23017 Atom keyboards.
// GPIO21 SDA, GPIO22 SCL. Proven released-line software I2C retained.
//
// Startup detection priority:
//   0x20 = FULL direct 8x8 keyboard (GPA0..7 rows, GPB0..7 columns)
//   0x21 = HACKJUNK legacy keyboard (proven V002 wiring)
//   0x22 = SMALL CTRL/SHIFT/REPT/BREAK/AUX1 keyboard (PORT10 behaviour)
//
// PC/serial keyboard support remains active for every keyboard type.

#include <Arduino.h>
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "atom_hw_kbd.h"

static const uint8_t SDA_PIN=21, SCL_PIN=22;
static const uint8_t ADDR_FULL=0x20, ADDR_HACKJUNK=0x21, ADDR_SMALL=0x22;
static const uint8_t IODIRA=0x00, IODIRB=0x01, IOCON=0x0A;
static const uint8_t GPPUA=0x0C, GPPUB=0x0D, GPIOA=0x12, GPIOB=0x13, OLATA=0x14;

enum KbdType : uint8_t { KBD_NONE, KBD_FULL, KBD_HACKJUNK, KBD_SMALL };
static KbdType kbd_type=KBD_NONE;
static uint8_t mcp_addr=0, hw_present=0;
static uint32_t next_scan_us=0;

// Cached Atom-side state. matrix[row] bits 0..5 are 1 while pressed.
static volatile uint8_t matrix_pressed[10]={0};
static volatile uint8_t cached_ctrl=0, cached_shift=0, cached_rept=0, cached_break=0;
static volatile uint8_t aux_pressed_event=0;

// V190 external-FE71 one-shot physical key event.
static volatile uint8_t fe_event_valid=0, fe_event_row=0, fe_event_col=0;
static volatile uint8_t fe_event_shift=0, fe_event_ctrl=0;
static volatile uint32_t fe_event_ms=0;
static const uint32_t FE_EVENT_MAX_MS=750;

// Debounce state for the Atom matrix and special keys.
static uint8_t db_raw[10][6]={{0}}, db_stable[10][6]={{0}};
static uint32_t db_changed[10][6]={{0}};
static uint8_t sp_raw[4]={0}, sp_stable[4]={0}; // CTRL SHIFT REPT BREAK
static uint32_t sp_changed[4]={0};
static uint8_t aux_raw=0, aux_stable=0;
static uint32_t aux_changed=0;

static inline void i2cDelay(){ delayMicroseconds(4); }
static inline void gpioDriveLow(uint8_t pin){ REG_WRITE(GPIO_OUT_W1TC_REG,(1UL<<pin)); REG_WRITE(GPIO_ENABLE_W1TS_REG,(1UL<<pin)); }
static inline void gpioRelease(uint8_t pin){ REG_WRITE(GPIO_ENABLE_W1TC_REG,(1UL<<pin)); }
static inline void sdaLow(){gpioDriveLow(SDA_PIN);} static inline void sdaRelease(){gpioRelease(SDA_PIN);}
static inline void sclLow(){gpioDriveLow(SCL_PIN);} static inline void sclRelease(){gpioRelease(SCL_PIN);}
static bool waitSclHigh(){ sclRelease(); uint32_t t=micros(); while(!digitalRead(SCL_PIN)) if((uint32_t)(micros()-t)>1000u) return false; return true; }
static bool i2cStart(){ sdaRelease(); if(!waitSclHigh()) return false; i2cDelay(); if(!digitalRead(SDA_PIN)) return false; sdaLow(); i2cDelay(); sclLow(); return true; }
static void i2cStop(){ sdaLow(); i2cDelay(); waitSclHigh(); i2cDelay(); sdaRelease(); i2cDelay(); }
static bool i2cWriteByte(uint8_t v){ for(uint8_t i=0;i<8;i++){ if(v&0x80)sdaRelease();else sdaLow(); i2cDelay(); if(!waitSclHigh())return false; i2cDelay(); sclLow(); v<<=1; } sdaRelease(); i2cDelay(); if(!waitSclHigh())return false; i2cDelay(); bool ack=!digitalRead(SDA_PIN); sclLow(); return ack; }
static uint8_t i2cReadByte(bool ack){ uint8_t v=0; sdaRelease(); for(uint8_t i=0;i<8;i++){v<<=1;i2cDelay();waitSclHigh();i2cDelay();if(digitalRead(SDA_PIN))v|=1;sclLow();} if(ack)sdaLow();else sdaRelease();i2cDelay();waitSclHigh();i2cDelay();sclLow();sdaRelease();return v; }
static bool mcpWrite(uint8_t reg,uint8_t value){ if(!i2cStart())return false; bool ok=i2cWriteByte((mcp_addr<<1)|0)&&i2cWriteByte(reg)&&i2cWriteByte(value);i2cStop();return ok; }
static bool mcpRead(uint8_t reg,uint8_t &v){ if(!i2cStart())return false;if(!i2cWriteByte((mcp_addr<<1)|0)||!i2cWriteByte(reg)){i2cStop();return false;}if(!i2cStart()){i2cStop();return false;}if(!i2cWriteByte((mcp_addr<<1)|1)){i2cStop();return false;}v=i2cReadByte(false);i2cStop();return true; }
static bool mcpRead2(uint8_t reg,uint8_t &a,uint8_t &b){ if(!i2cStart())return false;if(!i2cWriteByte((mcp_addr<<1)|0)||!i2cWriteByte(reg)){i2cStop();return false;}if(!i2cStart()){i2cStop();return false;}if(!i2cWriteByte((mcp_addr<<1)|1)){i2cStop();return false;}a=i2cReadByte(true);b=i2cReadByte(false);i2cStop();return true; }
static bool mcpPresentAt(uint8_t addr){ uint8_t save=mcp_addr;mcp_addr=addr;if(!i2cStart()){mcp_addr=save;return false;}bool ok=i2cWriteByte((addr<<1)|0);i2cStop();mcp_addr=save;return ok; }

static void latchFeEvent(uint8_t row,uint8_t col){ fe_event_row=row;fe_event_col=col;fe_event_shift=cached_shift;fe_event_ctrl=cached_ctrl;fe_event_ms=millis();fe_event_valid=1; }
static void updateMatrix(uint8_t row,uint8_t col,uint8_t pressed){
  uint32_t now=millis();
  if(pressed!=db_raw[row][col]){db_raw[row][col]=pressed;db_changed[row][col]=now;}
  if(db_stable[row][col]!=db_raw[row][col] && (uint32_t)(now-db_changed[row][col])>=20u){
    db_stable[row][col]=db_raw[row][col];
    if(db_stable[row][col]){ matrix_pressed[row]|=(uint8_t)(1u<<col); latchFeEvent(row,col); }
    else matrix_pressed[row]&=(uint8_t)~(1u<<col);
  }
}
static void updateSpecial(uint8_t which,uint8_t pressed){
  uint32_t now=millis(); if(pressed!=sp_raw[which]){sp_raw[which]=pressed;sp_changed[which]=now;}
  if(sp_stable[which]!=sp_raw[which] && (uint32_t)(now-sp_changed[which])>=20u) sp_stable[which]=sp_raw[which];
  cached_ctrl=sp_stable[0];cached_shift=sp_stable[1];cached_rept=sp_stable[2];cached_break=sp_stable[3];
}

// FULL 8x8: physical position -> Atom row/column. 0xFF = special/unfitted.
#define AC(r,c) ((uint8_t)(((r)<<4)|(c)))
static const uint8_t full_to_atom[8][8]={
 {AC(0,5),AC(2,1),AC(1,1),AC(0,1),AC(9,2),AC(8,2),AC(7,2),AC(6,2)},
 {AC(5,2),AC(4,2),AC(3,1),AC(0,2),AC(3,2),AC(5,0),0xFF,0xFF},
 {AC(3,0),AC(5,1),AC(0,4),AC(4,5),AC(2,3),AC(9,5),AC(7,5),AC(2,5)},
 {AC(6,5),AC(8,4),AC(2,4),AC(1,4),AC(7,3),AC(7,0),AC(4,1),0xFF},
 {AC(2,0),0xFF,AC(6,3),AC(8,5),AC(3,3),AC(1,3),AC(0,3),AC(9,4)},
 {AC(7,4),AC(6,4),AC(5,4),AC(2,2),AC(8,0),AC(6,0),AC(6,1),0xFF},
 {AC(4,0),0xFF,AC(1,5),AC(3,5),AC(4,3),AC(5,5),AC(5,3),AC(9,0)},
 {AC(3,4),AC(4,4),AC(1,2),AC(9,3),AC(8,3),0xFF,0xFF,0xFF}
};
#undef AC

static bool setupFull(){ return mcpWrite(IOCON,0)&&mcpWrite(IODIRA,0x00)&&mcpWrite(OLATA,0xFF)&&mcpWrite(IODIRB,0xFF)&&mcpWrite(GPPUA,0)&&mcpWrite(GPPUB,0xFF); }
static void scanFull(){
  for(uint8_t pr=0;pr<8;pr++){
    if(!mcpWrite(OLATA,(uint8_t)~(1u<<pr))){hw_present=0;return;} delayMicroseconds(20);
    uint8_t b;if(!mcpRead(GPIOB,b)){hw_present=0;return;}
    for(uint8_t pc=0;pc<8;pc++){
      uint8_t down=((b&(1u<<pc))==0);
      if(pr==1&&pc==6){updateSpecial(3,down);continue;} // BREAK
      if(pr==4&&pc==1){updateSpecial(0,down);continue;} // CTRL
      if((pr==6&&pc==1)||(pr==7&&pc==5)){ // either SHIFT
        // handled after row scan from stable physical state below
        static uint8_t shL=0,shR=0; if(pr==6)shL=down;else shR=down; updateSpecial(1,(uint8_t)(shL||shR)); continue;
      }
      if(pr==7&&pc==6){updateSpecial(2,down);continue;} // REPT
      uint8_t p=full_to_atom[pr][pc]; if(p!=0xFF) updateMatrix((uint8_t)(p>>4),(uint8_t)(p&15),down);
    }
  }
  mcpWrite(OLATA,0xFF);
}

// HACKJUNK exact proven V002 wiring.
static const uint8_t ATOM_PA_TO_GPA[4]={2,1,0,3};
static uint8_t hackMakeGPA(uint8_t pa){uint8_t out=0xF0;for(uint8_t bit=0;bit<4;bit++){uint8_t g=ATOM_PA_TO_GPA[bit];if(pa&(1u<<bit))out|=(1u<<g);else out&=(uint8_t)~(1u<<g);}return out;}
static bool setupHack(){return mcpWrite(IOCON,0)&&mcpWrite(IODIRA,0xF0)&&mcpWrite(IODIRB,0xFF)&&mcpWrite(GPPUA,0xF0)&&mcpWrite(GPPUB,0xFF)&&mcpWrite(OLATA,hackMakeGPA(15));}
static bool hackReadPB(uint8_t &pb){uint8_t a,b;if(!mcpRead2(GPIOA,a,b))return false;pb=0;if(a&(1u<<4))pb|=1u;for(uint8_t l=1;l<=7;l++)if(b&(1u<<(l-1)))pb|=(1u<<l);return true;}
static void scanHack(){
  for(uint8_t row=0;row<10;row++){if(!mcpWrite(OLATA,hackMakeGPA(row))){hw_present=0;return;}delayMicroseconds(20);uint8_t pb;if(!hackReadPB(pb)){hw_present=0;return;}for(uint8_t col=0;col<6;col++)updateMatrix(row,col,(pb&(1u<<col))?0:1);updateSpecial(0,(pb&(1u<<6))?0:1);updateSpecial(1,(pb&(1u<<7))?0:1);}
  mcpWrite(OLATA,hackMakeGPA(15));delayMicroseconds(20);uint8_t a,b;if(!mcpRead2(GPIOA,a,b)){hw_present=0;return;}updateSpecial(2,(b&(1u<<7))?0:1);updateSpecial(3,(a&(1u<<5))?0:1);
}

static bool setupSmall(){return mcpWrite(IOCON,0)&&mcpWrite(IODIRA,0xFD)&&mcpWrite(IODIRB,0xFF)&&mcpWrite(GPPUA,0xFD)&&mcpWrite(GPPUB,0xFF)&&mcpWrite(OLATA,0xFF);}
static void scanSmall(){
  if(!mcpWrite(OLATA,0xFF)){hw_present=0;return;}delayMicroseconds(20);uint8_t a,b;if(!mcpRead2(GPIOA,a,b)){hw_present=0;return;}
  updateSpecial(0,(b&(1u<<5))?0:1);updateSpecial(1,(b&(1u<<6))?0:1);uint8_t rept=(b&(1u<<7))?0:1;updateSpecial(2,rept);updateSpecial(3,(a&(1u<<5))?0:1);
  if(!mcpWrite(OLATA,0xFD)){hw_present=0;return;}delayMicroseconds(20);uint8_t ba;if(!mcpRead(GPIOB,ba)){hw_present=0;return;}uint8_t down=(!rept && ((ba&(1u<<7))==0));uint32_t now=millis();if(down!=aux_raw){aux_raw=down;aux_changed=now;}if(aux_stable!=aux_raw&&(uint32_t)(now-aux_changed)>=20u){uint8_t old=aux_stable;aux_stable=aux_raw;if(!old&&aux_stable)aux_pressed_event=1;}mcpWrite(OLATA,0xFF);
}

extern "C" int atom_hw_kbd_init(void){
  pinMode(SDA_PIN,INPUT_PULLUP);pinMode(SCL_PIN,INPUT_PULLUP);digitalWrite(SDA_PIN,LOW);digitalWrite(SCL_PIN,LOW);sdaRelease();sclRelease();delayMicroseconds(20);
  if(mcpPresentAt(ADDR_FULL)){kbd_type=KBD_FULL;mcp_addr=ADDR_FULL;}
  else if(mcpPresentAt(ADDR_HACKJUNK)){kbd_type=KBD_HACKJUNK;mcp_addr=ADDR_HACKJUNK;}
  else if(mcpPresentAt(ADDR_SMALL)){kbd_type=KBD_SMALL;mcp_addr=ADDR_SMALL;}
  else {Serial.println("[HW-KBD] no MCP23017 keyboard found at 0x20/0x21/0x22 on SDA=21 SCL=22");return 0;}
  bool ok=(kbd_type==KBD_FULL)?setupFull():(kbd_type==KBD_HACKJUNK)?setupHack():setupSmall();if(!ok){Serial.println("[HW-KBD] MCP23017 setup failed");return 0;}
  hw_present=1;next_scan_us=0;
  if(kbd_type==KBD_FULL) Serial.println("[HW-KBD] FULL DIRECT 8x8 MCP23017 OK at 0x20, SDA=21 SCL=22");
  else if(kbd_type==KBD_HACKJUNK) Serial.println("[HW-KBD] HACKJUNK MCP23017 OK at 0x21, SDA=21 SCL=22");
  else {Serial.println("[HW-KBD] SMALL MCP23017 OK at 0x22, SDA=21 SCL=22");Serial.println("[HW-KBD] PB5=CTRL PB6=SHIFT PB7=REPT PA5=BREAK AUX1=R1/C7");}
  Serial.println("[HW-KBD] PC/serial keyboard remains enabled");return 1;
}
extern "C" void atom_hw_kbd_scan(void){if(!hw_present)return;uint32_t n=micros();if((int32_t)(n-next_scan_us)<0)return;next_scan_us=n+2000u;if(kbd_type==KBD_FULL)scanFull();else if(kbd_type==KBD_HACKJUNK)scanHack();else scanSmall();}
extern "C" uint8_t atom_hw_kbd_get_port_b(uint8_t row){uint8_t v=0xFF;if(!hw_present)return v;if(row<10)v&=(uint8_t)~(matrix_pressed[row]&0x3F);if(cached_ctrl)v&=(uint8_t)~0x40u;if(cached_shift)v&=(uint8_t)~0x80u;return v;}
extern "C" uint8_t atom_hw_kbd_rept_pressed(void){return(hw_present&&cached_rept)?1u:0u;}
extern "C" uint8_t atom_hw_kbd_break_pressed(void){return(hw_present&&cached_break)?1u:0u;}
extern "C" uint8_t atom_hw_kbd_aux1_take_press(void){if(!aux_pressed_event)return 0;aux_pressed_event=0;return 1;}
extern "C" uint8_t atom_hw_kbd_take_fe71_event(uint8_t *row,uint8_t *col,uint8_t *shift,uint8_t *ctrl){if(!fe_event_valid)return 0;if((uint32_t)(millis()-fe_event_ms)>FE_EVENT_MAX_MS){fe_event_valid=0;return 0;}*row=fe_event_row;*col=fe_event_col;*shift=fe_event_shift;*ctrl=fe_event_ctrl;fe_event_valid=0;return 1;}
extern "C" void atom_hw_kbd_clear_fe71_event(void){fe_event_valid=0;}
