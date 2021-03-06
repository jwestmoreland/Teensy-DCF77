/**********************************************************************
   Copyright (c) 2016, Frank Bösing, f.boesing@gmx.de & Frank DD4WH, dd4wh.swl@gmail.com

    Teensy DCF77 Receiver & Real Time Clock

    uses only minimal hardware to receive accurate time signals

    For information on how to setup the antenna, see here:

    https://github.com/DD4WH/Teensy-DCF77/wiki
*/
#define VERSION     " v0.42b"					// updated - AJ6BC to work (pending) with WWVB (USA) - please see ReadMe.md for more detail.
/*
   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice, development funding notice, and this permission
   notice shall be included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.

*/

// #include <Time.h>		// warning message from Arduino compiler
#include <TimeLib.h>

#include <Audio.h>
#include <SPI.h>
#include <Metro.h>
#include <ILI9341_t3.h>
#include "font_Arial.h"

#include <utility/imxrt_hw.h>            // needed for setting I2S freq on the Teensy 4.x
#define SERIAL_COUNTER_TIME_OUT 5000    // so serial port doesn't hang the board
// #define DEBUG_DECODE 1
#define DEBUG_TELEGRAM_DECODE 1
#define DEBUG_DECODE_TELEGRAM 1
#define NO_SKIP_CHECK_FIXED 1
#define DEBUG 1
#define DEBUG_T 1
#define DEBUG_SIGNAL 1
#define DEBUG_SIGNAL_TO 1
#define MARKER_MASK (0x7fdff7fdff7fdfeULL)    // Markers are Zeroes - need to check
#define MARKER_AND_UNUSED_MASK (0x7bdec73dfc7f1feULL)    // Markers and Unused are Zeroes
#define DEBUG_SIGNAL_TO_MS 900       // signal time-out (?)         // 900
#define DEBUG_T_BOX 190    // min time window in ms, 190, 490, 790  // 80
#define DEBUG_DETECT_TO 10 // time out for calling detect bit routine in loop()
// #define DEBUG_PROCESSOR_CHECK 1  
// #define DEBUG_LOOP_CHECK 1
#define DEBUG_SIGNAL_DATA_SAMPLE_DUMP 1


time_t getTeensy3Time()
{
  return Teensy3Clock.get();
}

//#include <FlexiBoard.h>

#if 0
  #define BACKLIGHT_PIN
  #define TFT_DC      20
  #define TFT_CS      21
  #define TFT_RST     32  // 255 = unused. connect to 3.3V
  #define TFT_MOSI     7
  #define TFT_SCLK    14
  #define TFT_MISO    12
#endif

#define BACKLIGHT_PIN 22
#define TFT_DC      5
#define TFT_CS      14
#define TFT_RST     255  // 255 = unused. connect to 3.3V
#define TFT_MOSI     11
#define TFT_SCLK    13
#define TFT_MISO    12

ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC, TFT_RST, TFT_MOSI, TFT_SCLK, TFT_MISO);

#define SAMPLE_RATE_MIN               0
#define SAMPLE_RATE_8K                0
#define SAMPLE_RATE_11K               1
#define SAMPLE_RATE_16K               2
#define SAMPLE_RATE_22K               3
#define SAMPLE_RATE_32K               4
#define SAMPLE_RATE_44K               5
#define SAMPLE_RATE_48K               6
#define SAMPLE_RATE_88K               7
#define SAMPLE_RATE_96K               8
#define SAMPLE_RATE_176K              9
#define SAMPLE_RATE_192K              10
#define SAMPLE_RATE_MAX               10


AudioInputI2S            i2s_in;         //xy=202,411
AudioSynthWaveformSine   sine1;          //xy=354,249
AudioFilterBiquad        biquad1;        //xy=394,403
AudioEffectMultiply      mult1;          //xy=594,250
AudioFilterBiquad        biquad2;        //xy=761,248
//AudioAnalyzeFFT256       myFFT;          //xy=962,434
AudioAnalyzeFFT1024       myFFT;          //xy=962,434
AudioOutputI2S           i2s_out;        //xy=975,247
AudioConnection          patchCord1(i2s_in, 0, biquad1, 0);
AudioConnection          patchCord2(sine1, 0, mult1, 1);
AudioConnection          patchCord3(biquad1, 0, mult1, 0);
AudioConnection          patchCord4(biquad1, myFFT);
AudioConnection          patchCord5(mult1, biquad2);
AudioConnection          patchCord6(biquad2, 0, i2s_out, 1);
AudioConnection          patchCord7(biquad2, 0, i2s_out, 0);
AudioControlSGTL5000     sgtl5000_1;


// static unsigned long ltime = millis(); //Timer for loop

// Metro 1 second
Metro second_timer = Metro(1000);

const uint16_t FFT_points = 1024;
//const uint16_t FFT_points = 256;

int8_t mic_gain = 38 ;//start detecting with this MIC_GAIN in dB
const float bandpass_q = 10; // be careful when increasing Q, distortion can occur with higher Q because of fixed point 16bit math in the biquads
// const float DCF77_FREQ = 77500.0; //DCF-77 77.65 kHz
const float DCF77_FREQ = 60000.0; //DCF-77 77.65 kHz
// start detecting at this frequency, so that
// you can hear a 600Hz tone [77.5 - 76.9 = 0.6kHz]
unsigned int freq_real = DCF77_FREQ - 600;

//const unsigned int sample_rate = SAMPLE_RATE_176K;
//unsigned int sample_rate_real = 176400;
const unsigned int sample_rate = SAMPLE_RATE_192K;
unsigned int sample_rate_real = 192000;
unsigned long ltime = millis(); // Timer for loop

unsigned int freq_LO = 7000;
float dcf_signal = 0;
float dcf_signal_raw;
float dcf_threshold = 0;
float dcf_threshold_raw = 0;
float dcf_med = 0;
unsigned int DCF_bin;// this is the FFT bin where the 77.5kHz signal is
#ifdef DEBUG_SIGNAL_DATA_SAMPLE_DUMP
float dcf_signal_array[100];		// take 100 samples to examine
bool signal_ready_FLAG = false;
#endif
bool timeflag = 0;
const int8_t pos_x_date = 14;
const int8_t pos_y_date = 68;
const int8_t pos_x_time = 14;
const int8_t pos_y_time = 114;
uint8_t hour10_old;
uint8_t hour1_old;
uint8_t minute10_old;
uint8_t minute1_old;
uint8_t second10_old;
uint8_t second1_old;
uint8_t precision_flag = 0;
int8_t mesz = -1;
int8_t mesz_old = 0;

const float displayscale = 2.5;

typedef struct SR_Descriptor
{
  const int SR_n;
  const char* const f1;
  const char* const f2;
  const char* const f3;
  const char* const f4;
  const float32_t x_factor;
} SR_Desc;

// Text and position for the FFT spectrum display scale
const SR_Descriptor SR[SAMPLE_RATE_MAX + 1] =
{
  //   SR_n ,  f1, f2, f3, f4, x_factor = pixels per f1 kHz in spectrum display
  {  SAMPLE_RATE_8K,  " 1", " 2", " 3", " 4", 64.0}, // which means 64 pixels per 1 kHz
  {  SAMPLE_RATE_11K,  " 1", " 2", " 3", " 4", 43.1},
  {  SAMPLE_RATE_16K,  " 2", " 4", " 6", " 8", 64.0},
  {  SAMPLE_RATE_22K,  " 2", " 4", " 6", " 8", 43.1},
  {  SAMPLE_RATE_32K,  "5", "10", "15", "20", 80.0},
  {  SAMPLE_RATE_44K,  "5", "10", "15", "20", 58.05},
  {  SAMPLE_RATE_48K,  "5", "10", "15", "20", 53.33},
  {  SAMPLE_RATE_88K,  "10", "20", "30", "40", 58.05},
  {  SAMPLE_RATE_96K,  "10", "20", "30", "40", 53.33},
  {  SAMPLE_RATE_176K,  "20", "40", "60", "80", 58.05},
  {  SAMPLE_RATE_192K,  "20", "40", "60", "80", 53.33} // which means 53.33 pixels per 20kHz
};

//const int myInput = AUDIO_INPUT_LINEIN;
const int myInput = AUDIO_INPUT_MIC;

//const char* const Days[7] = { "Samstag", "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag"};
const char* const Days[7] = { "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday"};

void setup();
void loop();

//=========================================================================

void setup() {
  unsigned int counter_main = 0;
  uint64_t temp_var = 0;
  Serial.begin(115200);

#if 1
 // Serial.println();
 // Serial.println();

  // in case serial port isn't connected so we don't hang the program:
  do {
    counter_main++;

  } while ( !Serial && ( counter_main < SERIAL_COUNTER_TIME_OUT) );
#endif
  
  setSyncProvider(getTeensy3Time);

  // Audio connections require memory.
  AudioMemory(16);

  // Enable the audio shield. select input. and enable output
  sgtl5000_1.enable();
  sgtl5000_1.inputSelect(myInput);
  sgtl5000_1.volume(0.9);
  sgtl5000_1.micGain (mic_gain);
  sgtl5000_1.adcHighPassFilterDisable(); // does not help too much!

  // Init TFT display
  pinMode( BACKLIGHT_PIN, OUTPUT );
  analogWrite( BACKLIGHT_PIN, 1023 );
  tft.begin();
  tft.setRotation( 3 );
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(14, 7);
  tft.setTextColor(ILI9341_ORANGE);
  tft.setFont(Arial_12);
  tft.print("Teensy DCF77 Receiver "); tft.print(VERSION);
  tft.setTextColor(ILI9341_WHITE);
  display_settings();
  delay(2000);
  set_sample_rate (sample_rate);
  set_freq_LO (freq_real);
#if defined(DEBUG_DECODE_TELEGRAM)  
//   decodeTelegram( 0x8b47c0501a821b80ULL );
 //  decodeTelegram( 0xb040f0358530611ULL );
//     decodeTelegram( 0x1160f035853460bULL );
//       temp_var = (0x8860ca1ac0f020dULL & MARKER_AND_UNUSED_MASK);
    decodeTelegram( 0x840C218C07000CULL );
#endif
  displayDate();
  displayClock();
  displayPrecisionMessage();

  
  
} // END SETUP


void loop() {

#if defined(DEBUG_LOOP_TIME)	
	static unsigned long t = 1000;
	static unsigned long ttime = 1000;
#endif
	
  if (myFFT.available())
  {
	  agc();

	  if ( millis() - ltime >= DEBUG_DETECT_TO )		// call bit detection periodically
	  {
		  detectBit();
		  ltime = millis();
	  }
	  
    spectrum();
    displayClock();
#if defined(DEBUG_LOOP_TIME)	   
    ttime = millis();
    Serial.printf("%d\r\n", (ttime - t));
    t = millis();
#endif    
  }
#if defined(DEBUG_PROCESSOR_CHECK)  
      check_processor();
#endif
}

void set_mic_gain(int8_t gain) {
  // AudioNoInterrupts();
  sgtl5000_1.micGain (mic_gain);
  // AudioInterrupts();
  //  display_settings();
} // end function set_mic_gain

void       set_freq_LO(int freq) {
  // audio lib thinks we are still in 44118sps sample rate
  // therefore we have to scale the frequency of the local oscillator
  // in accordance with the REAL sample rate
  freq_LO = freq * (AUDIO_SAMPLE_RATE_EXACT / sample_rate_real);
  // if we switch to LOWER samples rates, make sure the running LO
  // frequency is allowed ( < 22k) ! If not, adjust consequently, so that
  // LO freq never goes up 22k, also adjust the variable freq_real
  if (freq_LO > 22000) {
    freq_LO = 22000;
    freq_real = freq_LO * (sample_rate_real / AUDIO_SAMPLE_RATE_EXACT) + 9;
  }
  AudioNoInterrupts();
  sine1.frequency(freq_LO);
  AudioInterrupts();
  //  display_settings();
} // END of function set_freq_LO

void      display_settings() {
  tft.fillRect(14, 32, 200, 17, ILI9341_BLACK);
  tft.setCursor(14, 32);
  tft.setFont(Arial_12);
  tft.print("gain: "); tft.print (mic_gain);
  tft.print("     ");
  tft.print("freq: "); tft.print (freq_real);
  tft.print("    ");
  tft.fillRect(232, 32, 88, 17, ILI9341_BLACK);
  tft.setCursor(232, 32);
  tft.print("       ");
  tft.print(sample_rate_real / 1000); tft.print("k");
}

void      set_sample_rate (int sr) {
  switch (sr) {
    case SAMPLE_RATE_8K:
      sample_rate_real = 8000;
      break;
    case SAMPLE_RATE_11K:
      sample_rate_real = 11025;
      break;
    case SAMPLE_RATE_16K:
      sample_rate_real = 16000;
      break;
    case SAMPLE_RATE_22K:
      sample_rate_real = 22050;
      break;
    case SAMPLE_RATE_32K:
      sample_rate_real = 32000;
      break;
    case SAMPLE_RATE_44K:
      sample_rate_real = 44100;
      break;
    case SAMPLE_RATE_48K:
      sample_rate_real = 48000;
      break;
    case SAMPLE_RATE_88K:
      sample_rate_real = 88200;
      break;
    case SAMPLE_RATE_96K:
      sample_rate_real = 96000;
      break;
    case SAMPLE_RATE_176K:
      sample_rate_real = 176400;
      break;
    case SAMPLE_RATE_192K:
      sample_rate_real = 192000;
      break;
  }
  AudioNoInterrupts();
//  sample_rate_real = setI2SFreq(sample_rate_real);
  setI2SFreq(sample_rate_real);
  delay(200); // this delay seems to be very essential !
  set_freq_LO (freq_real);

  // never set the lowpass freq below 1700 (empirically derived by ear ;-))
  // distortion will occur because of precision issues due to fixed point 16bit in the biquads
  biquad2.setLowpass(0, 1700, 0.54);
  biquad2.setLowpass(1, 1700, 1.3);
  biquad2.setLowpass(2, 1700, 0.54);
  biquad2.setLowpass(3, 1700, 1.3);

  biquad1.setBandpass(0, DCF77_FREQ * (AUDIO_SAMPLE_RATE_EXACT / sample_rate_real), bandpass_q);
  biquad1.setBandpass(1, DCF77_FREQ * (AUDIO_SAMPLE_RATE_EXACT / sample_rate_real), bandpass_q);
  biquad1.setBandpass(2, DCF77_FREQ * (AUDIO_SAMPLE_RATE_EXACT / sample_rate_real), bandpass_q);
  biquad1.setBandpass(3, DCF77_FREQ * (AUDIO_SAMPLE_RATE_EXACT / sample_rate_real), bandpass_q);

  AudioInterrupts();
  delay(20);
  DCF_bin = round((DCF77_FREQ / (sample_rate_real / 2.0)) * (FFT_points / 2));
  Serial.print("DCF77_bin number: "); Serial.println(DCF_bin);

  //  display_settings();
  prepare_spectrum_display();

} // END function set_sample_rate

void prepare_spectrum_display() {
  int base_y = 211;
  int b_x = 10;
  int x_f = SR[sample_rate].x_factor;
  tft.fillRect(0, base_y, 320, 240 - base_y, ILI9341_BLACK);
  //    tft.drawFastHLine(b_x, base_y + 2, 256, ILI9341_PURPLE);
  //    tft.drawFastHLine(b_x, base_y + 3, 256, ILI9341_PURPLE);
  tft.drawFastHLine(b_x, base_y + 2, 256, ILI9341_MAROON);
  tft.drawFastHLine(b_x, base_y + 3, 256, ILI9341_MAROON);
  // vertical lines
  tft.drawFastVLine(b_x - 4, base_y + 1, 10, ILI9341_YELLOW);
  tft.drawFastVLine(b_x - 3, base_y + 1, 10, ILI9341_YELLOW);
  tft.drawFastVLine( x_f + b_x,  base_y + 1, 10, ILI9341_YELLOW);
  tft.drawFastVLine( x_f + 1 + b_x,  base_y + 1, 10, ILI9341_YELLOW);
  tft.drawFastVLine( x_f * 2 + b_x,  base_y + 1, 10, ILI9341_YELLOW);
  tft.drawFastVLine( x_f * 2 + 1 + b_x,  base_y + 1, 10, ILI9341_YELLOW);
  if (x_f * 3 + b_x < 256 + b_x) {
    tft.drawFastVLine( x_f * 3 + b_x,  base_y + 1, 10, ILI9341_YELLOW);
    tft.drawFastVLine( x_f * 3 + 1 + b_x,  base_y + 1, 10, ILI9341_YELLOW);
  }
  if (x_f * 4 + b_x < 256 + b_x) {
    tft.drawFastVLine( x_f * 4 + b_x,  base_y + 1, 10, ILI9341_YELLOW);
    tft.drawFastVLine( x_f * 4 + 1 + b_x,  base_y + 1, 10, ILI9341_YELLOW);
  }
  tft.drawFastVLine( x_f * 0.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);
  tft.drawFastVLine( x_f * 1.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);
  tft.drawFastVLine( x_f * 2.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);
  if (x_f * 3.5 + b_x < 256 + b_x) {
    tft.drawFastVLine( x_f * 3.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);
  }
  if (x_f * 4.5 + b_x < 256 + b_x) {
    tft.drawFastVLine( x_f * 4.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);
  }
  // text
  tft.setTextColor(ILI9341_WHITE);
  tft.setFont(Arial_9);
  int text_y_offset = 16;
  int text_x_offset = - 5;
  // zero
  tft.setCursor (b_x + text_x_offset, base_y + text_y_offset);
  tft.print(0);
  tft.setCursor (b_x + x_f + text_x_offset, base_y + text_y_offset);
  tft.print(SR[sample_rate].f1);
  tft.setCursor (b_x + x_f * 2 + text_x_offset, base_y + text_y_offset);
  tft.print(SR[sample_rate].f2);
  tft.setCursor (b_x + x_f * 3 + text_x_offset, base_y + text_y_offset);
  tft.print(SR[sample_rate].f3);
  tft.setCursor (b_x + x_f * 4 + text_x_offset, base_y + text_y_offset);
  tft.print(SR[sample_rate].f4);
  //    tft.setCursor (b_x + text_x_offset + 256, base_y + text_y_offset);
  tft.print(" kHz");

  tft.setFont(Arial_14);
} // END prepare_spectrum_display


void agc() {
  static unsigned long tspeed = millis(); //Timer for startup

  const float speed_agc_start = 0.995;   //initial speed AGC
  const float speed_agc_run   = 0.9995;
  static float speed_agc = speed_agc_start;
  static unsigned long tagc = millis(); //Timer for AGC

  const float speed_thr = 0.995;

  static unsigned short i = 0;
  
  //  tft.drawFastHLine(14, 220 - dcf_med, 256, ILI9341_BLACK);
  tft.drawFastHLine(220, 220 - dcf_med, 46, ILI9341_BLACK);
  dcf_signal = (abs(myFFT.output[DCF_bin]) + abs(myFFT.output[DCF_bin + 1])) * displayscale;
#ifdef DEBUG_SIGNAL_DATA_SAMPLE_DUMP  
  if ( signal_ready_FLAG == false ) {            // an array of samples to dump
	  if ( i < 100 ) {
		  if ( dcf_signal > 800 )
		  dcf_signal_array[i++] = dcf_signal;
//		  Serial.print(".");
	  } else {
		  if ( i >= 100)
		  {
			  signal_ready_FLAG = true;
			  i = 0;
			  Serial.print("****");
		  }
	  }
  }
#endif
#if defined(DEBUG_SIGNAL)
  dcf_signal_raw = dcf_signal;
//  Serial.print("sig: "); Serial.print(dcf_signal);
#endif   
#if defined(DEBUG_SIGNAL) 
if (dcf_signal > 950) 
{
  dcf_signal  = 950;
  Serial.print(".");
}
#else
  if (dcf_signal > 175) dcf_signal  = 175;
#endif  
  else if (dcf_med == 0) dcf_med = dcf_signal;
  dcf_med = (1 - speed_agc) * dcf_signal + speed_agc * dcf_med;
  tft.drawFastHLine(220, 220 - dcf_med, 46, ILI9341_ORANGE);

  tft.drawFastHLine(220, 220 - dcf_threshold, 46, ILI9341_BLACK);
  dcf_threshold = (1 - speed_thr) * dcf_signal + speed_thr * dcf_threshold;
#if defined(DEBUG_SIGNAL)
  dcf_threshold_raw = dcf_threshold;
//  Serial.print("sig: "); Serial.print(dcf_signal);
#endif      
  tft.drawFastHLine(220, 220 - dcf_threshold, 46, ILI9341_GREEN);

  unsigned long t = millis();
  //Slow down speed after a while
  if ((t - tspeed > 1500) && (t - tspeed < 3500) ) {
    if (speed_agc < speed_agc_run) {
      speed_agc = speed_agc_run;
      Serial.printf("Set AGC-Speed %f\n", speed_agc);
    }
  }

  if ((t - tagc > 2221) || (speed_agc == speed_agc_start)) {
    tagc = t;
    if ((dcf_med > 160) && (mic_gain > 30)) {
      mic_gain--;
      set_mic_gain(mic_gain);
      Serial.printf("(Gain: %d)", mic_gain);
    }
//    if ((dcf_med < 100) && (mic_gain < 58)) {
      if ((dcf_med < 100) && (mic_gain < 63)) {
      mic_gain++;
      set_mic_gain(mic_gain);
      Serial.printf("(Gain: %d)", mic_gain);
    }
  }
}

int getParity(uint32_t value) {
  int par = 0;
  while (value) {
    value = value & (value - 1);
    par = ~par;
  }
  return par & 1;
}

int decodeTelegram(uint64_t telegram) {
  uint16_t minute, hour, day, weekday, month, year, v10;
  int parity;

//  uint64_t *telegram_ptr  = &telegram;

  //Plausibility checks and decoding telegram
  //Example-Data: 0x8b47c14f468f9ec0ULL : 2016/11/20

  //https://de.wikipedia.org/wiki/DCF77

  //TODO : more plausibility-checks to prevent false positives

  //TODO:  WWVB Compatability - https://en.wikipedia.org/wiki/WWVB

#if defined(NO_SKIP_CHECK_FIXED)
  //Check fixed - bits:
  if ( (((telegram >> 10)& 0x0f) != 0) || (((telegram >> 11)& 0x0f) != 0) || (((telegram >> 20)& 0x0f) != 0) || (((telegram >> 21)& 0x0f) != 0)\
  || (((telegram >> 34)& 0x0f) != 0) || (((telegram >> 35)& 0x0f) != 0) || (((telegram >> 44)& 0x0f) != 0) || (((telegram >> 54)& 0x0f) != 0) ) 
   {         // 10,11,20,21,34,35,44,54 always zero
    Serial.println("Unused-Bit error\n");
#if defined(DEBUG_DECODE_TELEGRAM)  
    Serial.printf(":0x%x:0x%x\r\n", ((telegram >> 10)& 0x0f), ((telegram >> 11)& 0x0f));
    Serial.printf(":0x%x:0x%x\r\n", ((telegram >> 20)& 0x0f), ((telegram >> 21)& 0x0f));
    Serial.printf(":0x%x:0x%x\r\n", ((telegram >> 34)& 0x0f), ((telegram >> 35)& 0x0f));
    Serial.printf(":0x%x:0x%x\r\n", ((telegram >> 44)& 0x0f), ((telegram >> 54)& 0x0f));
//    Serial.printf(":%x0x:%x0x\r\n", (*(telegram_ptr+10)& 0x0f) , (*(telegram_ptr+11)& 0x0f));
#else
   return 0;
#endif
  
  }
#endif

#if false  
  //MESZ Central European Summer Time ?
  mesz = (telegram >> 17) & 1;
  if ( mesz != (~(telegram >> 18) & 1) ) {                // signed and unsigned warning here
    Serial.println("MESZ-Bit error\n");
    return 0;
  }
#endif
  
  //1. decode date & date-parity-bit
#if false  
  parity = telegram >> 58 & 0x01;
  if (getParity( (telegram >> 36) & 0x3fffff) != parity) return 0;
#endif  
//  year = ((telegram >> 54) & 0x0f) * 10 + ((telegram >> 50) & 0x0f);
  year = ((telegram >> 48) & 0x0f) * 10 + ((telegram >> 47) & 0x0f) * 20 + ((telegram >> 46) & 0x0f) * 40 + ((telegram >> 45) & 0x0f) * 80;  // 00-99 is range for encoded year
  year = year + ((telegram >> 53) & 0x0f) + ((telegram >> 54) & 0x0f) * 2 + ((telegram >> 55) & 0x0f) * 4 + ((telegram >> 56) & 0x0f) * 8;
#if defined(DEBUG)
  Serial.print("year: "); Serial.println(year);
  if ( year > 99 )
    Serial.println("Error - encoded year range is only 00-99.");
//  Serial.printf("(year: %d)\r\n", year);
#endif
#if defined(DEBUG_DECODE_TELEGRAM)
         ;
#else
  if (year < 16) return 0;
#endif

#if false  
  month = ((telegram >> 45) & 0x0f);
  if (month > 9) return 0;

  month = ((telegram >> 49) & 0x01) * 10 + month;
  if ((month == 0) || (month > 12)) return 0;

  
  weekday = ((telegram >> 42) & 0x07);
  if (weekday == 0) return 0;

  day = ((telegram >> 36) & 0x0f);
  if (day > 9) return 0;
  day = ((telegram >> 40) & 0x03) * 10 + day;
  if ( (day == 0) || (day > 31) ) return 0;//Todo add check on 29.feb, 30/31 and more...
#endif

#if defined(DEBUG)
  month = 11;
  day = 1;
  weekday = 1;
#endif
  
  //2. decode time & parity-bit
#if false  
  parity = telegram >> 35 & 0x01;
  if  (getParity( (telegram >> 29) & 0x3f) != parity) return 0;
#endif  
//  hour = (telegram >> 29 & 0x0f);
  hour = (telegram >> 18 & 0x0f);
#if defined(DEBUG)
  Serial.print("hour: "); Serial.println(hour);
//  Serial.printf("\r\n");
//  Serial.printf("(hour: %d)\r\n", hour);
#endif  
  if (hour > 9) return 0;
//  v10 = (telegram >> 33 & 0x03);
  v10 = (telegram >> 13 & 0x03);
#if defined(DEBUG)
  Serial.print("v10: "); Serial.println(v10);
//  Serial.printf("(v10: %d)\r\n", v10);
#endif  
  if (v10 > 2) return 0;
  hour = v10 * 10 + hour;
#if defined(DEBUG)
  Serial.print("hourT: "); Serial.println(hour);
//  Serial.printf("(hourT: %d)\r\n", hour);
#endif    
  if (hour > 23) return 0;

#if false  
  parity = telegram >> 28 & 0x01;
  if (getParity( (telegram >> 21) & 0x7f ) != parity) return 0;
#endif
//  minute = (telegram >> 21 & 0x0f);
  minute = (telegram >> 8 & 0x0f) + (telegram >> 7 & 0x0f) * 2 + (telegram >> 6 & 0x0f) * 4 + (telegram >> 5 & 0x0f) * 8 + (telegram >> 3 & 0x0f) * 10 + (telegram >> 2 & 0x0f) * 20 + (telegram >> 1 & 0x0f) * 40;  // 00-59
#if defined(DEBUG)  
  Serial.print("minute: "); Serial.println(minute);
  if ( minute > 59 )
  Serial.println("Error:  minute > 59");
#endif  
#if false   // maybe only intervals are decoded... hence the 'incomplete' calculations...(?)
  if (minute > 9) return 0;
//  v10 = (telegram >> 25 & 0x07);
  v10 = (telegram >> 3 & 0x07);
#if defined(DEBUG)  
  Serial.print("v10m: "); Serial.println(v10);
#endif  
  if (v10 > 5) return 0;
  minute = v10 * 10 + minute;
#if defined(DEBUG)  
  Serial.print("minuteT: "); Serial.println(minute);
#endif  
#endif
  if (minute > 59) return 0;


  //All data seem to be ok.
  Serial.printf("Time set: %d.%d.20%d %d:%02d %s\n", day, month, year, hour, minute, mesz ? "MESZ" : "MEZ");
#if defined(DEBUG_DECODE_TELEGRAM)
      ;
#else  
  setTime (hour, minute, 0, day, month, year);
#endif    
  
  Teensy3Clock.set(now());
  displayDate();
  return 1;
}

void displayPrecisionMessage() {
  if (precision_flag) {
    tft.fillRect(14, 32, 300, 18, ILI9341_BLACK);
    tft.setCursor(14, 32);
    tft.setFont(Arial_11);
    tft.setTextColor(ILI9341_GREEN);
    tft.print("Full precision of time and date");
    tft.drawRect(290, 4, 20, 20, ILI9341_GREEN);
  }
  else
  {
    tft.fillRect(14, 32, 300, 18, ILI9341_BLACK);
    tft.setCursor(14, 32);
    tft.setFont(Arial_11);
    tft.setTextColor(ILI9341_RED);
    tft.print("Unprecise, trying to collect data");
    tft.drawRect(290, 4, 20, 20, ILI9341_RED);
  }
} // end function displayPrecisionMessage

int decode(unsigned long t) {
  static uint64_t data = 0;
  static int sec = 0;
  static unsigned long tlastBit = 0;
  int bit;
  unsigned long m;
  static unsigned char last_bit = 0;
  static unsigned char this_bit = 0;
  static bool M_FLAG = false;        // set if we get 2 Markers in a row
  static short missed_bit = 0;
  static bool got_bit_flag = false;

  m = millis();
#if defined(DEBUG_SIGNAL_TO)  
//  if ( m - tlastBit > 2000 ) {        
  if ( m - tlastBit > DEBUG_SIGNAL_TO_MS ) {  
#else
  if ( m - tlastBit > 1600) {                 // 1.6s?
#endif
 
#if defined(DEBUG_DECODE)
    Serial.printf(" End Of Telegram. Data: 0x%llx %d Bits\n", data, sec);
#endif   
//    Serial.print("-"); 
    tft.fillRect(14, 54, 59 * 5, 3, ILI9341_BLACK);
//    if (sec >= 59) {    
    if (sec == 59) {
      precision_flag = decodeTelegram(data);
      displayPrecisionMessage();
    }

    sec = 0;
    data = 0;
  }
  tlastBit = m;

  missed_bit = 1;
  
  // Official patterns:
  //     Zero is 200ms low, 800ms high
  //     One  is 500ms low, 500ms high
  //     Mark is 800ms low, 200ms high
 
 if ( t <= 210 && t >= 90 )
  {
	  bit = 0;
	  got_bit_flag = true;
	  missed_bit = 0;
  } else {
    missed_bit = 1;
  } 

  if ( t >= 490 && t < 790 )
  {
	  bit = 1;
	  got_bit_flag = true;
	  if (missed_bit) 
		  missed_bit = 0;
  } else {
	          if ( !got_bit_flag )
		  missed_bit = 1;	  
  }
  
   if ( t >= 790 )
  {
        bit = 'M';                         // beginning pattern - look for two in row
        if ( last_bit == bit )  // 2 M's
		M_FLAG = true;          // set flag
	
		 missed_bit = 0;
//      bit = 0x4D;
//      bit = 0x2;                        // let 2 == M for Marker Frame
  } else {
	  if (!got_bit_flag )
	   missed_bit = 1;
  }

//  bit = (t > 150) ? 1 : 0;          // (This denontes DCF77 'low' time.)  does this work for WWVB?  Have to allow for markers
///  bit = (t > 300) ? 1 : 0;
// #if defined(DEBUG_DECODE)  
//  Serial.print(bit);
#if defined(DEBUG_T)
    Serial.printf("\r\n");
    if ( bit == 'M' ) {
  if (M_FLAG == true)
    {
//    Serial.print("*Start of minute?*-> ");
    Serial.print("*SOM?*-> ");
    }   
   
    Serial.print(" bit: "); Serial.print('M');
    
    } else {
    Serial.print(" bit: "); Serial.print(bit);  
    }
    Serial.print(" <-t: "); Serial.print(t);
    Serial.print(" raw: "); Serial.print(dcf_signal_raw);
    Serial.print(" med: "); Serial.print(dcf_med);
    Serial.print(" thres: "); Serial.print(dcf_threshold_raw);
    Serial.print(" sec: "); Serial.print(sec);
    if ( missed_bit )
    {
	    Serial.print(" missed bit: "); Serial.print(missed_bit);
	    missed_bit = 0;
	    got_bit_flag = false;
    }
#ifdef DEBUG_SIGNAL_DATA_SAMPLE_DUMP
    unsigned short i = 0;
///    Serial.print("+");
    if ( signal_ready_FLAG == true )
    {
//	    Serial.print("-");
	    Serial.printf("\r\n");
	    while ( i < 100 )
	    {
		    Serial.printf("\t data :");
		    Serial.print(dcf_signal_array[i++]);
	    }

//	    Serial.print("+");	    
	    signal_ready_FLAG = false;
    }    
#endif    
#endif  
//    Serial.print(t);
// #endif

  // plot horizontal bar
  tft.fillRect(14 + 5 * sec, 54, 3, 3, bit ? ILI9341_YELLOW : ILI9341_PURPLE);
  data = ( data >> 1) | ((uint64_t)bit << 58);

  sec++;
  if (sec > 59) { // just to prevent accidents with weak signals ;-)
//    Serial.println(sec);
    Serial.println("!");
    sec = 0;
  }
  last_bit = bit;
  M_FLAG = false;
  return bit;
}

void detectBit() {
  static float dcf_threshold_last = 1000;
  static unsigned long secStart = 0;

//  dcf_threshold = 250;          // bias this right now.

  if ( dcf_threshold <= dcf_threshold_last)
  {
    if (secStart == 0) {
      secStart = millis();
    }
  }

  else {
    unsigned long t = millis() - secStart;
    if ((secStart > 0) && (t > DEBUG_T_BOX )) {  // 200, 500, 800 -> 190, 490, 790
//    if ((secStart > 0) && (t > 90)) {     
  
      int bit = decode(t);
      
      tft.fillRect(291, 5, 18, 18, ILI9341_BLACK);
      tft.setFont(Arial_12);
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(295, 8);
      tft.print(bit);
    }
    secStart = 0;
  }
  dcf_threshold_last = dcf_threshold;
}


void spectrum() { // spectrum analyser code by rheslip - modified

  static int barm [512];

  for (unsigned int x = 2; x < FFT_points / 2; x++) {
    int bar = abs(myFFT.output[x]) * (int)(displayscale * 2.0);
    if (bar > 175) bar = 175;

    // this is a very simple first order IIR filter to smooth the reaction of the bars
    bar = 0.05 * bar + 0.95 * barm[x];
    tft.drawPixel(x / 2 + 10, 210 - barm[x], ILI9341_BLACK);
    tft.drawPixel(x / 2 + 10, 210 - bar, ILI9341_WHITE);
    barm[x] = bar;
  }
} // end void spectrum


#if 0
int setI2SFreq(int freq) {
  typedef struct {
    uint8_t mult;
    uint16_t div;
  } tmclk;

 // int I2S0_MDR, I2S_MCR_DUF, I2S0_MCR;
  const int numfreqs = 14;
  const int samplefreqs[numfreqs] = { 8000, 11025, 16000, 22050, 32000, 44100, (int)44117.64706 , 48000, 88200, (int)44117.64706 * 2, 96000, 176400, (int)44117.64706 * 4, 192000};

// int clkArr[numfreqs];

#if (F_PLL==16000000)
  const tmclk clkArr[numfreqs] = {{16, 125}, {148, 839}, {32, 125}, {145, 411}, {64, 125}, {151, 214}, {12, 17}, {96, 125}, {151, 107}, {24, 17}, {192, 125}, {127, 45}, {48, 17}, {255, 83} };
#elif (F_PLL==72000000)
  const tmclk clkArr[numfreqs] = {{32, 1125}, {49, 1250}, {64, 1125}, {49, 625}, {128, 1125}, {98, 625}, {8, 51}, {64, 375}, {196, 625}, {16, 51}, {128, 375}, {249, 397}, {32, 51}, {185, 271} };
#elif (F_PLL==96000000)
  const tmclk clkArr[numfreqs] = {{8, 375}, {73, 2483}, {16, 375}, {147, 2500}, {32, 375}, {147, 1250}, {2, 17}, {16, 125}, {147, 625}, {4, 17}, {32, 125}, {151, 321}, {8, 17}, {64, 125} };
#elif (F_PLL==120000000)
  const tmclk clkArr[numfreqs] = {{32, 1875}, {89, 3784}, {64, 1875}, {147, 3125}, {128, 1875}, {205, 2179}, {8, 85}, {64, 625}, {89, 473}, {16, 85}, {128, 625}, {178, 473}, {32, 85}, {145, 354} };
#elif (F_PLL==144000000)
  const tmclk clkArr[numfreqs] = {{16, 1125}, {49, 2500}, {32, 1125}, {49, 1250}, {64, 1125}, {49, 625}, {4, 51}, {32, 375}, {98, 625}, {8, 51}, {64, 375}, {196, 625}, {16, 51}, {128, 375} };
#elif (F_PLL==168000000)
  const tmclk clkArr[numfreqs] = {{32, 2625}, {21, 1250}, {64, 2625}, {21, 625}, {128, 2625}, {42, 625}, {8, 119}, {64, 875}, {84, 625}, {16, 119}, {128, 875}, {168, 625}, {32, 119}, {189, 646} };
#elif (F_PLL==180000000)
  const tmclk clkArr[numfreqs] = {{46, 4043}, {49, 3125}, {73, 3208}, {98, 3125}, {183, 4021}, {196, 3125}, {16, 255}, {128, 1875}, {107, 853}, {32, 255}, {219, 1604}, {214, 853}, {64, 255}, {219, 802} };
#elif (F_PLL==192000000)
  const tmclk clkArr[numfreqs] = {{4, 375}, {37, 2517}, {8, 375}, {73, 2483}, {16, 375}, {147, 2500}, {1, 17}, {8, 125}, {147, 1250}, {2, 17}, {16, 125}, {147, 625}, {4, 17}, {32, 125} };
#elif (F_PLL==216000000)
  const tmclk clkArr[numfreqs] = {{32, 3375}, {49, 3750}, {64, 3375}, {49, 1875}, {128, 3375}, {98, 1875}, {8, 153}, {64, 1125}, {196, 1875}, {16, 153}, {128, 1125}, {226, 1081}, {32, 153}, {147, 646} };
#elif (F_PLL==240000000)
  const tmclk clkArr[numfreqs] = {{16, 1875}, {29, 2466}, {32, 1875}, {89, 3784}, {64, 1875}, {147, 3125}, {4, 85}, {32, 625}, {205, 2179}, {8, 85}, {64, 625}, {89, 473}, {16, 85}, {128, 625} };
#endif

  for (int f = 0; f < numfreqs; f++) {
    if ( freq == samplefreqs[f] ) {
      while (I2S0_MCR & I2S_MCR_DUF) ;
      I2S0_MDR = I2S_MDR_FRACT(((tmclk) clkArr[f].mult - 1)) | I2S_MDR_DIVIDE(((tmclk) clkArr[f].div - 1));
      return round(((float)F_PLL / 256.0) * clkArr[f].mult / clkArr[f].div); //return real freq
    }
  }
  return 0;
}
#endif
// set samplerate code by Frank Bösing
void setI2SFreq(int freq) {
  // PLL between 27*24 = 648MHz und 54*24=1296MHz
  int n1 = 4; //SAI prescaler 4 => (n1*n2) = multiple of 4
  int n2 = 1 + (24000000 * 27) / (freq * 256 * n1);
  double C = ((double)freq * 256 * n1 * n2) / 24000000;
  int c0 = C;
  int c2 = 10000;
  int c1 = C * c2 - (c0 * c2);
  set_audioClock(c0, c1, c2, true);
  CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK | CCM_CS1CDR_SAI1_CLK_PODF_MASK))
       | CCM_CS1CDR_SAI1_CLK_PRED(n1-1) // &0x07
       | CCM_CS1CDR_SAI1_CLK_PODF(n2-1); // &0x3f

//START//Added afterwards to make the SAI2 function at the desired frequency as well.
  
CCM_CS2CDR = (CCM_CS2CDR & ~(CCM_CS2CDR_SAI2_CLK_PRED_MASK | CCM_CS2CDR_SAI2_CLK_PODF_MASK))
       | CCM_CS2CDR_SAI2_CLK_PRED(n1-1) // &0x07
       | CCM_CS2CDR_SAI2_CLK_PODF(n2-1); // &0x3f)

//END//Added afterwards to make the SAI2 function at the desired frequency as well.
}

void check_processor() {
    if (second_timer.check() == 1) {
    Serial.println("");
    Serial.print("Proc = ");
    Serial.print(AudioProcessorUsage());
    Serial.print(" (");
    Serial.print(AudioProcessorUsageMax());
    Serial.print("),  Mem = ");
    Serial.print(AudioMemoryUsage());
    Serial.print(" (");
    Serial.print(AudioMemoryUsageMax());
    Serial.println(")");
    /*      tft.fillRect(100,120,200,80,ILI9341_BLACK);
          tft.setCursor(10, 120);
          tft.setTextSize(2);
          tft.setTextColor(ILI9341_WHITE);
          tft.setFont(Arial_14);
          tft.print ("Proc = ");
          tft.setCursor(100, 120);
          tft.print (AudioProcessorUsage());
          tft.setCursor(180, 120);
          tft.print (AudioProcessorUsageMax());
          tft.setCursor(10, 150);
          tft.print ("Mem  = ");
          tft.setCursor(100, 150);
          tft.print (AudioMemoryUsage());
          tft.setCursor(180, 150);
          tft.print (AudioMemoryUsageMax());
    */
    AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
  }
} // END function check_processor

void displayClock() {

  uint8_t hour10 = hour() / 10 % 10;
  uint8_t hour1 = hour() % 10;
  uint8_t minute10 = minute() / 10 % 10;
  uint8_t minute1 = minute() % 10;
  uint8_t second10 = second() / 10 % 10;
  uint8_t second1 = second() % 10;
  uint8_t time_pos_shift = 26;
  tft.setFont(Arial_28);
  tft.setTextColor(ILI9341_WHITE);
  uint8_t dp = 14;

  if (mesz != mesz_old && mesz >= 0) {
    tft.setTextColor(ILI9341_ORANGE);
    tft.setFont(Arial_16);    
    tft.setCursor(pos_x_date, pos_y_date+20);
    tft.fillRect(pos_x_date, pos_y_date+20, 150-pos_x_date, 20, ILI9341_BLACK);
    tft.printf((mesz==0)?"(CET)":"(CEST)");
  }

  tft.setFont(Arial_28);
  tft.setTextColor(ILI9341_WHITE);

  // set up ":" for time display
  if (!timeflag) {
    tft.setCursor(pos_x_time + 2 * time_pos_shift, pos_y_time);
    tft.print(":");
    tft.setCursor(pos_x_time + 4 * time_pos_shift + dp, pos_y_time);
    tft.print(":");
    tft.setCursor(pos_x_time + 7 * time_pos_shift + 2 * dp, pos_y_time);
    //      tft.print("UTC");
  }

  if (hour10 != hour10_old || !timeflag) {
    tft.setCursor(pos_x_time, pos_y_time);
    tft.fillRect(pos_x_time, pos_y_time, time_pos_shift, time_pos_shift + 2, ILI9341_BLACK);
    if (hour10) tft.print(hour10);  // do not display, if zero
  }
  if (hour1 != hour1_old || !timeflag) {
    tft.setCursor(pos_x_time + time_pos_shift, pos_y_time);
    tft.fillRect(pos_x_time  + time_pos_shift, pos_y_time, time_pos_shift, time_pos_shift + 2, ILI9341_BLACK);
    tft.print(hour1);  // always display
  }
  if (minute1 != minute1_old || !timeflag) {
    tft.setCursor(pos_x_time + 3 * time_pos_shift + dp, pos_y_time);
    tft.fillRect(pos_x_time  + 3 * time_pos_shift + dp, pos_y_time, time_pos_shift, time_pos_shift + 2, ILI9341_BLACK);
    tft.print(minute1);  // always display
  }
  if (minute10 != minute10_old || !timeflag) {
    tft.setCursor(pos_x_time + 2 * time_pos_shift + dp, pos_y_time);
    tft.fillRect(pos_x_time  + 2 * time_pos_shift + dp, pos_y_time, time_pos_shift, time_pos_shift + 2, ILI9341_BLACK);
    tft.print(minute10);  // always display
  }
  if (second10 != second10_old || !timeflag) {
    tft.setCursor(pos_x_time + 4 * time_pos_shift + 2 * dp, pos_y_time);
    tft.fillRect(pos_x_time  + 4 * time_pos_shift + 2 * dp, pos_y_time, time_pos_shift, time_pos_shift + 2, ILI9341_BLACK);
    tft.print(second10);  // always display
  }
  if (second1 != second1_old || !timeflag) {
    tft.setCursor(pos_x_time + 5 * time_pos_shift + 2 * dp, pos_y_time);
    tft.fillRect(pos_x_time  + 5 * time_pos_shift + 2 * dp, pos_y_time, time_pos_shift, time_pos_shift + 2, ILI9341_BLACK);
    tft.print(second1);  // always display
  }

  hour1_old = hour1;
  hour10_old = hour10;
  minute1_old = minute1;
  minute10_old = minute10;
  second1_old = second1;
  second10_old = second10;
  mesz_old = mesz;
  timeflag = 1;

} // end function displayTime

void displayDate() {
  char string99 [20];
  tft.fillRect(pos_x_date, pos_y_date, 320 - pos_x_date, 20, ILI9341_BLACK); // erase old string
  tft.setTextColor(ILI9341_ORANGE);
  tft.setFont(Arial_16);
  tft.setCursor(pos_x_date, pos_y_date);
  //  Date: %s, %d.%d.20%d P:%d %d", Days[weekday-1], day, month, year
  sprintf(string99, "%s, %02d.%02d.%04d", Days[weekday()], day(), month(), year());
  tft.print(string99);
} // end function displayDate
