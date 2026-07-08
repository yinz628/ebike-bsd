// C3 v2.9 — WiFi到SYS页 + 残影更彻底修复
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Wire.h>

Adafruit_ST7789 tft(4,6,-1);
#define DK 0x39E7
#define BG ST77XX_BLACK
#define ACC 0x5B9E
#define RED 0xF8E0
#define GRN 0x2E44
#define YEL 0xFFE0
#define WH ST77XX_WHITE

struct Target { int8_t angle; uint8_t range; int8_t velocity; };
Target tgt[4],tgt_old[4]; int tgt_n,tgt_n_old;
bool bsd,rcw; int bz,turn;

#define CX 160
#define CY 28
#define MAXR 165
#define DETRANGE 40
bool bg_drawn=false, inCfg=false;
int tx=0,ty=0; bool touched=false;
unsigned long last_rx_ms=0;        // 最后一次收到ESP32数据的时间戳
#define CONN_TOUT 1500             // 超过1.5秒无数据视为断开

struct {
  int rcw_low=2,rcw_speed=3,rcw_range=25,rcw_hold=3000,rcw_lflash=500,rcw_flash=125;
  int turn_speed=2,turn_range=30,turn_hold=3000,turn_flash=125;
  int sys_beep=5000,radar_range=30,radar_sens=2;
  bool wifi_on=true;
} cfg;
int pg=0; // 0=RCW1,1=RCW2,2=TURN,3=SYS
unsigned long saveMsg=0;

#define TOUCH 0x38

bool readTouch(int *x,int *y){
  Wire.beginTransmission(TOUCH);Wire.write(0x02);
  if(Wire.endTransmission(false)!=0)return false;
  Wire.requestFrom(TOUCH,(uint8_t)6);if(Wire.available()<6)return false;
  uint8_t buf[6];for(int i=0;i<6;i++)buf[i]=Wire.read();
  if(buf[0]==0)return false;
  *x=((buf[1]&0x0F)<<8)|buf[2];*y=((buf[3]&0x0F)<<8)|buf[4];
  int tmp=*x;*x=320-*y;*y=tmp;return true;
}
bool hit(int rx,int ry,int rw,int rh){return tx>=rx&&tx<=rx+rw&&ty>=ry&&ty<=ry+rh;}

void btn(int x,int y,int w,int h,const char* t,uint16_t c,int sz=2){
  tft.fillRoundRect(x,y,w,h,5,c);tft.setTextColor(WH);tft.setTextSize(sz);
  tft.setCursor(x+(w-sz*6*strlen(t))/2,y+(h-sz*8)/2);tft.print(t);
}

void patchBg(int sx,int sy){
  float base=PI/2,half=60*PI/180,a0=base-half,b40=40*PI/180;
  // 加大检测范围 50x45
  for(int j=1;j<=4;j++){int R=MAXR*j/4;
    for(int deg=0;deg<=120;deg+=3){float a=a0+deg*PI/180;int x=CX+R*cos(a),y=CY+R*sin(a);
      if(abs(x-sx)<50&&abs(y-sy)<45)tft.drawPixel(x,y,0x31A6);}}
  float ls[]={base-b40,base+b40,base};uint16_t cs[]={ST77XX_BLUE,ST77XX_BLUE,DK};
  for(int k=0;k<3;k++)for(int r=0;r<MAXR;r+=6){int px=CX+r*cos(ls[k]),py=CY+r*sin(ls[k]);
    if(abs(px-sx)<50&&abs(py-sy)<45)tft.drawPixel(px,py,cs[k]);}
}

void drawBg(){
  tft.fillScreen(BG);
  float base=PI/2,half=60*PI/180,a0=base-half,a1=base+half,b40=40*PI/180;
  for(int j=1;j<=4;j++){int R=MAXR*j/4;
    for(int deg=0;deg<=120;deg+=3){float a=a0+deg*PI/180;tft.drawPixel(CX+R*cos(a),CY+R*sin(a),0x31A6);}
    tft.setTextSize(1);tft.setTextColor(DK);float la=a0-0.1;
    tft.setCursor(CX+MAXR*j/4*cos(la)-30,CY+MAXR*j/4*sin(la)+2);tft.printf("%dm",DETRANGE*j/4);}
  for(int r=4;r<MAXR;r+=6){tft.drawPixel(CX+r*cos(base-b40),CY+r*sin(base-b40),ST77XX_BLUE);
    tft.drawPixel(CX+r*cos(base+b40),CY+r*sin(base+b40),ST77XX_BLUE);tft.drawPixel(CX+r*cos(base),CY+r*sin(base),DK);}
  tft.fillTriangle(CX-6,CY-10,CX,CY-20,CX+6,CY-10,ST77XX_CYAN);tft.fillRect(CX-4,CY-10,8,8,ST77XX_CYAN);
  tft.setTextSize(1);tft.setCursor(2,2);tft.print("BSD v2.9");
  // 连接状态指示（初始:断开）
  tft.fillCircle(52,8,4,RED);tft.setTextColor(RED);tft.setCursor(60,2);tft.print("LOST");
  btn(250,2,65,22,"SET",ST77XX_BLUE,2);
  tft.setTextColor(cfg.wifi_on?GRN:RED);tft.setCursor(210,5);tft.print(cfg.wifi_on?"WiFi":"NO_WiFi");
  bg_drawn=true;
}

void pRow(int y,const char* label,int val,const char* unit){
  tft.setTextSize(2);tft.setTextColor(WH);tft.setCursor(5,y);tft.print(label);
  btn(115,y-2,44,26,"-",RED,2);btn(161,y-2,44,26,"+",GRN,2);
  tft.setCursor(215,y);tft.print(val);tft.setTextSize(1);tft.setTextColor(DK);tft.setCursor(270,y+4);tft.print(unit);
}

void drawConfig(){
  tft.fillScreen(BG);
  btn(2,2,44,26,"<",ACC,2);
  const char* ts[]={"RCW 1/2","RCW 2/2","TURN","SYS"};
  tft.setTextSize(2);tft.setTextColor(WH);tft.setCursor(60,4);tft.print(ts[pg]);
  btn(274,2,44,26,">",ACC,2);
  tft.drawLine(0,30,319,30,DK);
  
  if(millis()-saveMsg<2000){
    tft.setTextSize(3);tft.setTextColor(GRN);
    tft.setCursor(60,100);tft.print("SAVED!");
  }
  
  int16_t y=34;
  if(pg==0){
    pRow(y,"LOW_V",cfg.rcw_low,"m/s");y+=30;
    pRow(y,"HI_V",cfg.rcw_speed,"m/s");y+=30;
    pRow(y,"RANGE",cfg.rcw_range,"m");y+=30;
    pRow(y,"HOLD",cfg.rcw_hold,"ms");y+=30;
    pRow(y,"L_FLSH",cfg.rcw_lflash,"ms");y+=30;
    pRow(y,"H_FLSH",cfg.rcw_flash,"ms");
  }else if(pg==1){
    pRow(y,"BEEP_CD",cfg.sys_beep,"ms");
  }else if(pg==2){
    pRow(y,"SPEED",cfg.turn_speed,"m/s");y+=30;
    pRow(y,"RANGE",cfg.turn_range,"m");y+=30;
    pRow(y,"HOLD",cfg.turn_hold,"ms");y+=30;
    pRow(y,"FLASH",cfg.turn_flash,"ms");
  }else{
    pRow(y,"RAD_RNG",cfg.radar_range,"m");y+=30;
    pRow(y,"SENS",cfg.radar_sens,"");y+=30;
    pRow(y,"BEEP_CD",cfg.sys_beep,"ms");y+=30;
    // WiFi 开关
    tft.setTextSize(2);tft.setTextColor(WH);tft.setCursor(5,y);tft.print("WiFi");
    btn(115,y-2,90,26,cfg.wifi_on?"ON":"OFF",cfg.wifi_on?GRN:RED,2);
  }
  
  btn(5,214,70,26,"BACK",RED,1);
  btn(80,214,70,26,"REFRESH",ACC,1);
  btn(155,214,120,26,"SAVE->ESP",GRN,1);
}

void sendCfg(){
  char buf[200];
  snprintf(buf,sizeof(buf),"$CFG,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
    cfg.rcw_low,cfg.rcw_speed,cfg.rcw_range,cfg.rcw_hold,cfg.rcw_lflash,cfg.rcw_flash,
    cfg.turn_speed,cfg.turn_range,cfg.sys_beep,cfg.radar_range,cfg.radar_sens);
  Serial1.print(buf);
  saveMsg=millis();
  drawConfig();
}

void sendWiFi(){
  Serial1.printf("$WIFI,%d\n",cfg.wifi_on?1:0);
  saveMsg=millis();
  drawConfig();
}

void getCfg(){
  Serial1.print("$GETCFG\n");
}

void parseCfg(const char* line){
  int v[11]={0};
  if(sscanf(line,"$CFG,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
    &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9],&v[10])>=9){
    cfg.rcw_low=v[0]; cfg.rcw_speed=v[1]; cfg.rcw_range=v[2];
    cfg.rcw_hold=v[3]; cfg.rcw_lflash=v[4]; cfg.rcw_flash=v[5];
    cfg.turn_speed=v[6]; cfg.turn_range=v[7];
    cfg.sys_beep=v[8];
    cfg.radar_range=v[9]; cfg.radar_sens=v[10];
    saveMsg=millis();
    drawConfig();
  }
}

int chkRow(){
  int16_t by=34;int mx;
  if(pg==0)mx=6;else if(pg==1)mx=1;else if(pg==2)mx=4;else mx=4;
  for(int i=0;i<mx;i++){if(hit(115,by-2,44,26)||hit(161,by-2,44,26))return i;by+=30;}
  return -1;
}

void handleTouch(){
  if(!inCfg)return;
  int row=chkRow();if(row<0)return;
  bool plus=hit(161,34+row*30-2,44,26);
  if(pg==0){int st[]={1,1,5,500,100,25};int* v[]={&cfg.rcw_low,&cfg.rcw_speed,&cfg.rcw_range,&cfg.rcw_hold,&cfg.rcw_lflash,&cfg.rcw_flash};if(row<6)*v[row]+=(plus?st[row]:-st[row]);}
  else if(pg==1){if(row==0)cfg.sys_beep+=(plus?500:-500);}
  else if(pg==2){int st[]={1,5,500,25};int* v[]={&cfg.turn_speed,&cfg.turn_range,&cfg.turn_hold,&cfg.turn_flash};if(row<4)*v[row]+=(plus?st[row]:-st[row]);}
  else{if(row<3){int st[]={5,1,500};int* v[]={&cfg.radar_range,&cfg.radar_sens,&cfg.sys_beep};*v[row]+=(plus?st[row]:-st[row]);}else{if(!plus){cfg.wifi_on=!cfg.wifi_on;sendWiFi();}}}
  drawConfig();
}

void setup(){
  delay(3000);
  pinMode(2,OUTPUT);digitalWrite(2,LOW);
  SPI.begin(3,-1,5);
  tft.init(240,320);tft.setRotation(1);tft.fillScreen(BG);tft.setTextWrap(false);
  Wire.begin(0,1);Wire.setClock(100000);
  Serial.begin(115200);Serial1.begin(115200,SERIAL_8N1,19,18);
}

void loop(){
  bool upd=false;
  while(Serial1.available()){
    // 先检查是否是文本命令 ('$' = 0x24)
    int peek = Serial1.peek();
    if(peek == '$'){
      String line = Serial1.readStringUntil('\n');
      line.trim();
      if(line.startsWith("$CFG,")){
        parseCfg(line.c_str());
        upd=true;
      }
      continue;
    }
    uint8_t b=Serial1.read();if(b!=0xAA)continue;
    unsigned long t0=millis();while(millis()-t0<15&&!Serial1.available());
    if(!Serial1.available())continue;
    uint8_t type=Serial1.read(),buf[12];int n=0;t0=millis();
    while(n<12&&millis()-t0<20){if(Serial1.available())buf[n++]=Serial1.read();}
    if(type==0xFF&&n>=4){bsd=buf[0];rcw=buf[1];bz=buf[2];turn=buf[3];upd=true;last_rx_ms=millis();}
    else if(type<=4&&n>=type*3){tgt_n=type;for(int i=0;i<type&&i<4;i++){tgt[i].angle=(int8_t)buf[i*3];tgt[i].range=buf[i*3+1];tgt[i].velocity=(int8_t)buf[i*3+2];}upd=true;last_rx_ms=millis();}
  }
  
  touched=readTouch(&tx,&ty);
  static bool lastT=false;
  if(touched&&!lastT){
    if(!inCfg){
      if(hit(250,2,65,22)){inCfg=true;drawConfig();upd=true;}
    }else{
      if(hit(5,214,70,26)){inCfg=false;bg_drawn=false;drawBg();upd=true;}
      else if(hit(80,214,70,26))getCfg();
      else if(hit(155,214,120,26))sendCfg();
      else if(hit(2,2,44,26)){pg=(pg+3)%4;drawConfig();}
      else if(hit(274,2,44,26)){pg=(pg+1)%4;drawConfig();}
      else handleTouch();
    }
  }
  lastT=touched;
  
  static unsigned long last=0;
  if(upd||millis()-last>200){last=millis();draw();}
  yield();
}

void draw(){
  if(inCfg)return;
  if(!bg_drawn)drawBg();
  // 连接状态指示（动态更新）
  bool conn=(millis()-last_rx_ms<CONN_TOUT);
  tft.fillRect(52,0,80,14,BG);                          // 清除旧指示
  tft.fillCircle(52,8,4,conn?GRN:RED);                   // 绿点=已连 红点=断开
  tft.setTextSize(1);tft.setTextColor(conn?GRN:RED);
  tft.setCursor(60,2);tft.print(conn?"ON":"LOST");
  // 加大清除矩形 (50+50=100宽, 覆盖任何标签)
  for(int i=0;i<tgt_n_old;i++){float ar=tgt_old[i].angle*PI/180;int r=(int)((float)tgt_old[i].range/DETRANGE*MAXR);if(r>MAXR)r=MAXR;
    int ox=constrain(CX+r*sin(ar),6,314),oy=constrain(CY+r*cos(ar),10,CY+MAXR+10);
    tft.fillRect(ox-50,oy-25,100,50,BG);
    patchBg(ox,oy);}
  for(int i=0;i<tgt_n;i++){float ar=tgt[i].angle*PI/180;int r=(int)((float)tgt[i].range/DETRANGE*MAXR);if(r>MAXR)r=MAXR;
    int px=constrain(CX+r*sin(ar),4,316),py=constrain(CY+r*cos(ar),10,CY+MAXR+10);int sz=((millis()/400)%2)?5:4;
    tft.fillCircle(px,py,sz+1,tft.color565(248,81,73));tft.fillCircle(px,py,sz,ST77XX_RED);
    tft.setCursor(constrain(px-12,2,290),py-4);tft.setTextColor(WH);tft.printf("%dm/s",tgt[i].velocity);}
  String s;uint16_t sc;if(turn){s="TURN";sc=YEL;}else if(rcw){s="RCW!";sc=RED;}else if(bsd){s="BSD";sc=YEL;}
  else{s="IDLE";sc=GRN;}tft.setTextColor(sc);tft.setCursor(2,228);tft.print(s);
  if(bz){tft.setTextColor(WH);tft.setCursor(55,228);tft.printf("BZ:%d",bz);}
  tgt_n_old=tgt_n;for(int i=0;i<tgt_n;i++)tgt_old[i]=tgt[i];
}
