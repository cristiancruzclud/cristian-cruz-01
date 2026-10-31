/*
  MONITOR DE PULSO ESP32 + SENSOR DE PULSO ANALOGICO + ST7789 240x280 - v1
  (sin MAX30102; no mide SpO2)

  Deteccion de latidos por PENDIENTE de subida (mas robusta que el umbral
  sobre la amplitud): ignora la deriva lenta y la onda dicrotica.

  CONEXIONES (ESP32 clasico, por ejemplo ESP32 Dev Module):
  TFT: SCL=18, SDA=23, RES=4, DC=2, CS=5, VCC/BLK=3.3V, GND=GND.
  SENSOR DE PULSO: Positivo(VCC)=3V3, Negativo(GND)=GND, Senal(S)=GPIO34.

  Bibliotecas (gestor de Arduino): Adafruit GFX Library,
  Adafruit ST7735 and ST7789 Library (acepta BusIO).
  Serial: 115200 baudios.

  MODO_PLOTTER=true : Herramientas > Serial Plotter (series: Pulso, BPM).
                      BPM=0 significa dato NO disponible.
  MODO_PLOTTER=false: Monitor Serie con texto y el valor crudo (Raw).

  USO: apoya la yema del dedo sobre el sensor, suave y sin moverte.
  El primer BPM tarda unos 5-8 s. Con el sensor sin dedo debe verse
  "--". Prototipo educativo: no es un dispositivo medico.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

constexpr int TFT_CS=5, TFT_DC=2, TFT_RST=4;
constexpr int PULSE_PIN=34;
constexpr bool MODO_PLOTTER=true;

constexpr uint32_t PERIODO_MS=10;          // 100 Hz
constexpr uint32_t REFRACTARIO_MS=300;
constexpr uint32_t INTERVALO_MIN_MS=333;   // 180 BPM
constexpr uint32_t INTERVALO_MAX_MS=1500;  // 40 BPM
constexpr uint32_t CADUCIDAD_MS=3000;
constexpr float AMPLITUD_MIN=30.0f;        // cuentas ADC (0-4095)
constexpr int N_INTERVALOS=6;
constexpr float ALFA_SUAVE=0.20f;          // suavizado de la senal
constexpr float ALFA_PEND=0.25f;           // suavizado de la pendiente
constexpr float FACTOR_UMBRAL=0.60f;       // latido = 60 % de la subida maxima
constexpr float REARME=0.20f;              // se rearma al bajar la pendiente
constexpr float DEC_PEND=0.997f;           // caida lenta del pico de pendiente
constexpr float DESV_MAX=0.25f;            // regularidad maxima de intervalos
constexpr float ALFA_GRAF=0.15f;           // filtro de la grafica (2 etapas, mas suave)
constexpr float ESCALA_MIN=25.0f;          // escala minima: evita amplificar el ruido

constexpr int GX=10, GY=164, GW=220, GH=88;
constexpr uint16_t FONDO=ST77XX_BLACK, REJILLA=0x18E3, MAGENTA_A=0xF81F;

Adafruit_ST7789 tft(TFT_CS,TFT_DC,TFT_RST);

uint32_t ultimoMuestreo=0, ultimaUI=0, ultimoTexto=0;
uint32_t ultimoLatido=0;
int rawA=0;
float baseA=0, suaveA=0, bandaA=0, bandaPrevA=0, picoA=0;
float onda=0;
bool filtroListo=false, activo=false;
float bpm=-1;
bool bpmValido=false;
uint32_t intervalos[N_INTERVALOS];
int nIntervalos=0, idxIntervalo=0;
float prevSuaveA=0, pendA=0, pendPrevA=0, picoPendA=0, medianaMs=0;
bool armado=true;
float graf1=0, graf2=0, grafBase=0, grafPico=0;

int xGrafica=0, yAnterior=GY+GH/2;
bool hayPunto=false;
uint8_t divisorGrafica=0;
int bpmPintado=-999;
bool latidoPintado=false;
char estadoPintado[40]="";

void dibujarBase();
void limpiarGrafica();
void dibujarColumna(int x);
void graficar(float normalizada);
void muestra();
void reiniciarBPM();
void actualizarUI();
void salidaSerial();

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(PULSE_PIN,ADC_11db);
  pinMode(PULSE_PIN,INPUT);
  SPI.begin(18,-1,23,TFT_CS);
  tft.init(240,280);
  tft.setRotation(0);
  tft.setTextWrap(false);
  dibujarBase();
  actualizarUI();
  ultimoMuestreo=millis();
}

void loop() {
  uint32_t ahora=millis();
  if (ahora-ultimoMuestreo>=PERIODO_MS) {
    ultimoMuestreo+=PERIODO_MS;              // periodo fijo de 10 ms (sin deriva)
    if (ahora-ultimoMuestreo>=3*PERIODO_MS) ultimoMuestreo=ahora;
    muestra();
  }
  if (ahora-ultimaUI>=50) {
    ultimaUI=ahora;
    actualizarUI();
  }
  if (!MODO_PLOTTER && ahora-ultimoTexto>=500) {
    ultimoTexto=ahora;
    salidaSerial();
  }
  delay(1);
}

void reiniciarBPM() {
  nIntervalos=idxIntervalo=0;
  medianaMs=0;
  ultimoLatido=0;
  bpm=-1; bpmValido=false;
}

void muestra() {
  uint32_t ahora=millis();

  // Promedio de 4 lecturas: el ADC del ESP32 es ruidoso
  long suma=0;
  for (int i=0;i<4;i++) suma+=analogRead(PULSE_PIN);
  rawA=int(suma/4);
  float x=float(rawA);

  if (!filtroListo) {
    baseA=suaveA=prevSuaveA=x; picoA=0;
    graf1=graf2=grafBase=x; grafPico=0;
    pendA=pendPrevA=picoPendA=0; armado=true;
    filtroListo=true;
  }
  suaveA+=ALFA_SUAVE*(x-suaveA);             // suavizado
  baseA+=0.02f*(suaveA-baseA);               // linea base lenta
  bandaA=suaveA-baseA;                       // componente pulsatil

  float absB=fabsf(bandaA);
  if (absB>picoA) picoA=absB; else picoA+=0.005f*(absB-picoA);
  activo=(picoA>AMPLITUD_MIN);

  // Onda de la pantalla: filtro propio de 2 etapas (quita el ruido rapido),
  // se resta la deriva lenta y se escala con un minimo fijo (no amplifica ruido)
  graf1+=ALFA_GRAF*(x-graf1);
  graf2+=ALFA_GRAF*(graf1-graf2);
  grafBase+=0.02f*(graf2-grafBase);
  float gb=graf2-grafBase;
  float ga=fabsf(gb);
  if (ga>grafPico) grafPico=ga; else grafPico+=0.01f*(ga-grafPico);
  float escala=fmaxf(grafPico,ESCALA_MIN);
  onda=activo ? constrain(0.45f+0.5f*gb/escala,0.0f,1.0f)*100.0f : 0.0f;

  // Pendiente de subida: el latido es la subida mas empinada de la senal.
  // Ignora la deriva lenta y la onda dicrotica (el segundo "bulto").
  pendA+=ALFA_PEND*((suaveA-prevSuaveA)-pendA);
  prevSuaveA=suaveA;
  float pp=fmaxf(pendA,0.0f);
  if (pp>picoPendA) picoPendA=pp; else picoPendA*=DEC_PEND;
  float umbral=FACTOR_UMBRAL*picoPendA;
  if (!armado && pendA<REARME*picoPendA) armado=true;

  // Refractario adaptativo: 60 % del intervalo habitual (minimo 300 ms)
  uint32_t refr=REFRACTARIO_MS;
  if (bpmValido && bpm>0) refr=max(REFRACTARIO_MS,uint32_t(0.6f*60000.0f/bpm));
  else if (medianaMs>0)   refr=max(REFRACTARIO_MS,uint32_t(0.6f*medianaMs));

  if (activo && armado && pendPrevA<umbral && pendA>=umbral &&
      (ahora-ultimoLatido)>refr) {
    armado=false;
    if (ultimoLatido!=0) {
      uint32_t iv=ahora-ultimoLatido;
      if (iv>=INTERVALO_MIN_MS && iv<=INTERVALO_MAX_MS) {
        intervalos[idxIntervalo]=iv;
        idxIntervalo=(idxIntervalo+1)%N_INTERVALOS;
        if (nIntervalos<N_INTERVALOS) nIntervalos++;
        uint32_t c[N_INTERVALOS];
        memcpy(c,intervalos,sizeof(c));
        for (int i=1;i<nIntervalos;i++) {            // ordenar (mediana)
          uint32_t k=c[i]; int j=i-1;
          while (j>=0 && c[j]>k) { c[j+1]=c[j]; j--; }
          c[j+1]=k;
        }
        float med=(nIntervalos%2) ? float(c[nIntervalos/2])
                 : 0.5f*float(c[nIntervalos/2-1]+c[nIntervalos/2]);
        medianaMs=med;
        if (nIntervalos>=3) {
          float dev=0;
          for (int i=0;i<nIntervalos;i++) dev+=fabsf(float(c[i])-med);
          dev/=nIntervalos;
          if (dev/med<=DESV_MAX) {                   // pulso regular
            float b=60000.0f/med;
            bpm=bpmValido ? 0.4f*b+0.6f*bpm : b;
            bpmValido=true;
          } else {                                   // ruido
            bpmValido=false; bpm=-1;
          }
        }
      } else if (iv>INTERVALO_MAX_MS) {
        nIntervalos=idxIntervalo=0; medianaMs=0;
      }
    }
    ultimoLatido=ahora;
  }
  pendPrevA=pendA;

  // Caducidad
  if (!activo) {
    reiniciarBPM();
  } else if (ultimoLatido!=0 && ahora-ultimoLatido>CADUCIDAD_MS) {
    reiniciarBPM();
  }

  // Grafica a 50 Hz (una de cada dos muestras)
  if (++divisorGrafica>=2) { divisorGrafica=0; graficar(onda/100.0f); }
  if (MODO_PLOTTER) salidaSerial();
}

// ---------------- PANTALLA ----------------
void dibujarBase() {
  tft.fillScreen(FONDO);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(12,8); tft.print("MONITOR DE PULSO");
  tft.drawFastHLine(10,32,220,REJILLA);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(12,42); tft.print("BPM");
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10,151); tft.print("ONDA DE PULSO");
  tft.drawRect(GX-1,GY-1,GW+2,GH+2,ST77XX_WHITE);
  tft.setTextColor(0x8410);
  tft.setCursor(10,264); tft.print("Sensor de pulso GPIO34 | Educativo");
  limpiarGrafica();
}

void dibujarColumna(int x) {
  tft.drawFastVLine(GX+x,GY,GH,(x%25==0)?REJILLA:FONDO);
  for (int y=22;y<GH;y+=22) tft.drawPixel(GX+x,GY+y,REJILLA);
}

void limpiarGrafica() {
  tft.fillRect(GX,GY,GW,GH,FONDO);
  for (int x=0;x<GW;x+=25) tft.drawFastVLine(GX+x,GY,GH,REJILLA);
  for (int y=22;y<GH;y+=22) tft.drawFastHLine(GX,GY+y,GW,REJILLA);
  xGrafica=0; yAnterior=GY+GH/2; hayPunto=false;
}

void graficar(float normalizada) {
  int y=GY+GH-3-int(normalizada*(GH-6));
  dibujarColumna(xGrafica);
  dibujarColumna((xGrafica+1)%GW);
  dibujarColumna((xGrafica+2)%GW);
  uint16_t color=activo?MAGENTA_A:REJILLA;
  if (hayPunto && xGrafica>0)
    tft.drawLine(GX+xGrafica-1,yAnterior,GX+xGrafica,y,color);
  else tft.drawPixel(GX+xGrafica,y,color);
  yAnterior=y; hayPunto=true;
  xGrafica=(xGrafica+1)%GW;
}

void actualizarUI() {
  uint32_t ahora=millis();
  int b=bpmValido?int(lroundf(bpm)):-1;
  char texto[12];
  if (b!=bpmPintado) {
    if (b<0) strcpy(texto,"-- "); else snprintf(texto,sizeof(texto),"%-3d",b);
    tft.setTextSize(6);
    tft.setTextColor(ST77XX_CYAN,FONDO);
    tft.setCursor(12,64); tft.print(texto);
    bpmPintado=b;
  }
  // Corazon que parpadea con cada latido
  bool latido=(ultimoLatido!=0 && ahora-ultimoLatido<150);
  if (latido!=latidoPintado) {
    tft.fillCircle(205,86,14,latido?ST77XX_RED:FONDO);
    latidoPintado=latido;
  }
  char estado[40];
  uint16_t color=ST77XX_WHITE;
  if (!activo) strcpy(estado,"Coloca el dedo sobre el sensor");
  else if (!bpmValido) { strcpy(estado,"Midiendo... manten el dedo quieto"); color=ST77XX_YELLOW; }
  else strcpy(estado,"Leyendo. Manten el dedo quieto");
  if (strcmp(estado,estadoPintado)!=0) {
    tft.fillRect(10,119,220,17,FONDO);
    tft.setTextSize(1); tft.setTextColor(color);
    tft.setCursor(10,123); tft.print(estado);
    strcpy(estadoPintado,estado);
  }
}

// ---------------- SERIAL ----------------
void salidaSerial() {
  char linea[100];
  int b=bpmValido?int(lroundf(bpm)):0;
  int n;
  if (MODO_PLOTTER)
    n=snprintf(linea,sizeof(linea),"Pulso:%.1f,BPM:%d\n",onda,b);
  else
    n=snprintf(linea,sizeof(linea),"Raw:%d Activo:%d Pico:%.0f BPM:%d\n",
       rawA,activo,picoA,b);
  if (n>0 && n<int(sizeof(linea)) && Serial.availableForWrite()>=n)
    Serial.write((const uint8_t*)linea,n);
}