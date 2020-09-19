/* SmartFireSensor R1 */
// Timer
HardwareTimer* Timer1;
bool timerFlag = true;
volatile uint8_t secCnt = 0, minCnt = 0;

// SoftwareSerial
#include <SoftwareSerial.h>
SoftwareSerial WifiSerial(10, 9); // RX, TX

// Sensor data collection completed
bool collected = false;

// SMOKE Sensor
#include <Wire.h>
#include "MAX30105.h"
volatile int smoke_val[3];
enum { R, IR, G };

// JSON Encode & Decode
#include <ArduinoJson.h>
#define DATA_DOC_CAPACITY JSON_OBJECT_SIZE(6) + JSON_ARRAY_SIZE(2)
#define JSON_DATA_BUFF_MAX_SIZE 200
uint8_t integrated[JSON_DATA_BUFF_MAX_SIZE];

// Uart serial
#define SERIAL_BUFF_MAX_SIZE 200
uint8_t serialBuff[SERIAL_BUFF_MAX_SIZE];
uint8_t* s_data = nullptr; // Data start point
volatile bool received = false;
uint8_t req_data;

// Data start & end signal
enum SE_SIGN {
	STX				= 0xBE, // Start data
	ETX				= 0xED // End data
};

// MCU to ESP Serial OP Code
enum OP_CODE {
	DATA			= 0x00,	// Data ack
	ACK				= 0x01,	// Positive ack
	REQ				= 0x02,	// Request data
	NAK				= 0xFE // Negative ack
};

// MCU to ESP Serial Data Type 
enum DATA_TYPE {
	NONE			= 0x00,	// None state
	AP				= 0x01,	// AP Mode
	STA				= 0x02,	// Station Mode
	AP_STA			= 0x03,	// AP & Station Mode
	SENSOR			= 0xF0, // Sensor data
	MODE_MASK		= 0xFC // Not value, used to split a mode from other data
};
uint8_t mcu_mode = NONE;
uint8_t esp_mode = NONE;
uint8_t req_mode = NONE;

MAX30105 particleSensor;

// LED & Switch
#define GLED A2 // PA3
#define RLED D6 // PB1
#define MODESW D2 // PA12
#define TESTSW D3 // PB0
volatile bool last=HIGH, curr=HIGH, ModeFlag = LOW; 
//boolean last=LOW, curr=LOW, ModeFlag=false;

// Fire detect
bool fire_detect = false;

// GAS Sensor
#define GAS_PIN A5 // PA6
volatile int reading;
// #define GAS_EN D12 // PB4, unused

// NTC Thermistor : NTC-103F397F (SAMKYOUNG Ceramic)
#define THERMISTOR_PIN A1 // PA1
volatile float Tc = 0;

// PIR Sensor
volatile byte pir_val[2];
#define PIR_PIN1 A3 // PA4
#define PIR_PIN2 A4 // PA5

// Dust Sensor
#define no_dust 0.35  // Initialize Voltage value 0.35
// Dust Sensor Interface
#define DUST_PIN A0 // PA0
#define DUST_EN A6 // PA7
volatile float dust_density = 0;

#if 0 // not used
// Variables will change:
int buttonState;             // the current reading from the input pin
int lastButtonState = LOW;   // the previous reading from the input pin

// the following variables are unsigned longs because the time, measured in
// milliseconds, will quickly become a bigger number than can be stored in an int.
unsigned long lastDebounceTime = 0;  // the last time the output pin was toggled
unsigned long debounceDelay = 50;    // the debounce time; increase if the output flickers
#endif
#define debug Serial //Uncomment this line if you're using an Uno or ESP
//#define debug SerialUSB //Uncomment this line if you're using a SAMD21
//#define toESP WifiSerial
#define toESP Serial

// Function pre-define
bool parseData();
void sendToESP(const char op_code, const uint8_t* send_data, size_t data_len);
void sendToESP(const char op_code, const char send_data);
void req_process(const uint8_t cur_mode, const uint8_t req_data);
size_t dataToJson();
bool mcuSetMode(const uint8_t req_mode);
void ap_init(); 
void sta_init(); 
void checkModeSw();
boolean debounce(boolean last);
void timerTask();
bool checkSensor();
void runFunc();
void checkPir(); 
void checkSmoke();
void checkGas();
void readTemp();
void readDust();
void setup_gpio();
void setup_timer();


void setup_gpio() {
	// PIR Sensor
	pinMode(PIR_PIN1, INPUT);
	pinMode(PIR_PIN2, INPUT);

	// LED
	pinMode(GLED, OUTPUT);
	digitalWrite(GLED, HIGH);
	pinMode(RLED, OUTPUT);
	digitalWrite(RLED, HIGH);

	// AP <-> Station Mode Switch 
	pinMode(MODESW, INPUT);
	// pinMode(TESTSW, INPUT);
}

void setup_timer() {
	TIM_TypeDef* ins = TIM1;
	Timer1 = new HardwareTimer(ins);
	Timer1-> setOverflow(1, HERTZ_FORMAT); // 1HZ
	Timer1-> attachInterrupt(timerTask);
}

void setup()
{
	debug.begin(9600);
	WifiSerial.begin(9600);

	debug.println(F("Smart Fire Sensor R1"));

	// Initialize sensor
	if (particleSensor.begin() == false)
	{
		debug.println(F("MAX30105 was not found. Please check wiring/power. "));
		while (1);
	}

	particleSensor.setup(); //Configure sensor. Use 6.4mA for LED drive

	// PIN setup
	setup_gpio();

	// Timer setup
	setup_timer();
}

void loop()
{
	checkModeSw();
	if (ModeFlag == HIGH) {
		uint8_t chg_mode = (mcu_mode|STA)^AP;
		sendToESP(REQ, chg_mode);
		digitalWrite(GLED, LOW);  
		delay(250);
		digitalWrite(GLED, HIGH);
		delay(250);
		ModeFlag = LOW;
	}

	// Display current mode state
	digitalWrite(GLED, !(mcu_mode & AP));

	// Send the sensor data requested to MQTT server
	if (received == true) {
		bool requested = parseData();
		if (requested) { req_process(mcu_mode, req_data); }
		memset(serialBuff, 0, SERIAL_BUFF_MAX_SIZE);
		received = false;
	}
	//debug
	//debug.printf("mcu: %u / esp: %u / req: %u\n", mcu_mode, esp_mode, req_mode);

	if (mcu_mode != req_mode) {	if(!(mcuSetMode(req_mode))) { goto FAIL; } }

	if (mcu_mode == STA) {
		if(minCnt == 15) { // Get sensor data per 15 minute
			timerFlag = true;
			minCnt = 0;
		}

		if (timerFlag)
		{
			digitalWrite(RLED, LOW);   // Turn the red LED on
			runFunc();
			digitalWrite(RLED, HIGH);   // Turn the red LED off
			timerFlag = false;
		}
#if 0
		bool abnormal = checkSensor();
		// Current circumstance may be emergency
		// therefore, send alarm signal to App of user's smartphone
		if (abnormal == true) {
			digitalWrite(RLED, !abnormal); // Turn the red LED on
			runFunc();
			/*
			 * TODO: data processing & send data & send alarm to App
			 */
			// Send emergency data to MQTT server
			if (1) { // TODO:send cnt... umm
				size_t json_len = dataToJson();
				sendToESP(DATA, integrated, json_len); // TODO: All data send???
			}

			abnormal = false; // ???
			digitalWrite(RLED, !abnormal); // Turn the red LED off
		}
#endif
		// Station mode end
	} else if (mcu_mode & AP) {
		// AP mode end
	}

FAIL:
	delay(500);
}

void ap_init() {
	Timer1-> pause();
	secCnt = 0;
	minCnt = 0;
	timerFlag = true;
}

void sta_init() {
	Timer1-> resume();
}

// MCU mode setup
bool mcuSetMode(const uint8_t req_mode) {
	if (req_mode & AP) { // AP & AP_STA
		ap_init(); // AP mode setup
	} else if (req_mode == STA) { // STA
		sta_init(); // Station mode setup
	} else { return false; } // NONE

	mcu_mode = req_mode;
	sendToESP(ACK, mcu_mode);
	esp_mode = mcu_mode;

	return true;
}

// Process requested data
void req_process(const uint8_t cur_mode, const uint8_t req_data) {
	if ((cur_mode == STA) && (req_data == SENSOR)) {
		if (!collected) { runFunc(); }
		size_t json_len = dataToJson();
		sendToESP(DATA, integrated, json_len);
	} 
}

// Received request for sensor data from MQTT server
void serialEvent() {
	size_t cnt = 0;

	while (toESP.available() > 0) {
		char get_data = toESP.read();
		serialBuff[cnt++] = get_data;
		if (get_data == ETX || cnt >= SERIAL_BUFF_MAX_SIZE) {
			received = true;
			return;
		}
	}
}

bool parseData() {
	size_t cnt = 0;
	bool _requested = false;

	uint8_t* p_data = serialBuff;
	// Start sign search
	while (*(p_data++) != STX)
		if ((cnt++) >= SERIAL_BUFF_MAX_SIZE) { return false; }

	uint8_t _op_code = *(p_data++);
	size_t _data_len = *(p_data++) << 8 | *(p_data++);
	if (p_data[_data_len] != ETX) { return false; }

	s_data = p_data;

	if (_op_code == REQ) { // REQ
		req_data = *s_data;
		_requested = true;
	} else if (_op_code == ACK) { // ACK
		if (!(*s_data & MODE_MASK)) { req_mode = *s_data; } // Mode(AP/Station) ACK
		else { /* Data ACK */ }
	} else if (_op_code == NAK) { /* NAK */ }
	else { /* DATA */ }

	return _requested;
}

// Send data stream in serial to ESP32
void sendToESP(const char op_code, const uint8_t* send_data, size_t data_len) {
	char len_high = data_len >> 8;
	char len_low = data_len & 0xFF;

	toESP.write(STX);
	toESP.write(op_code);
	toESP.write(len_high);
	toESP.write(len_low);
	toESP.write(send_data, data_len);
	toESP.write(ETX);
}

// Send data in serial to ESP32
void sendToESP(const char op_code, const char send_data) {
	toESP.write(STX);
	toESP.write(op_code);
	toESP.write(0);
	toESP.write(1);
	toESP.write(send_data);
	toESP.write(ETX);
}

// Send sensor data to ESP32
size_t dataToJson() {
	StaticJsonDocument<DATA_DOC_CAPACITY> Data;
	Data["dust"] = dust_density;
//	Data["fire"] = reading & Smoke;
	Data["gas"] = reading;
	JsonArray Motion = Data.createNestedArray("motion");
	Motion.add(pir_val[0]);
	Motion.add(pir_val[1]);
	Data["smoke"] = smoke_val[R];
	Data["temp"] = Tc;

	memset(integrated, 0, JSON_DATA_BUFF_MAX_SIZE);
	return serializeJson(Data, integrated);
}

void checkModeSw()
{
	curr=debounce(last);
	if (last == HIGH && curr == LOW) {
		ModeFlag = HIGH;
	}
	last=curr;
}

// Switch Debounce
boolean debounce(boolean last)
{
	boolean curr=digitalRead(MODESW);

	if (last!=curr) {
		delay(500);
		curr=digitalRead(MODESW);
	}

	return curr;
}

// Timer interrupt
void timerTask()
{
	++secCnt;
	if (secCnt == 60) {
		++minCnt;
		secCnt = 0;
	}
}

bool checkSensor()
{
	pir_val[0] = digitalRead(PIR_PIN1);
	pir_val[1] = digitalRead(PIR_PIN2);
	smoke_val[R] = particleSensor.getRed();
	int reading = analogRead(GAS_PIN);

	bool _abnormal = (pir_val[0] == 1 || pir_val[1] == 1 || \ 
						smoke_val[R] >= 10000 || reading >= 1000); // ??? exactly

	return _abnormal? true : false;
}

void runFunc()
{
	checkPir();
	checkSmoke();
	checkGas();
	readTemp();
	readDust();
	collected = true;
}

// Check PIR1, PIR2
void checkPir() 
{
	pir_val[0] = digitalRead(PIR_PIN1);
	pir_val[1] = digitalRead(PIR_PIN2);
#if 1
	debug.print(pir_val[0]);
	debug.print(pir_val[1]);
#endif
}

void checkSmoke()
{
	smoke_val[R] = particleSensor.getRed();
	smoke_val[IR] = particleSensor.getIR();
	smoke_val[G] = particleSensor.getGreen();
#if 1
	debug.print(F(" R["));
	debug.print(smoke_val[R]);
	debug.print(F("] IR["));
	debug.print(smoke_val[IR]);
	debug.print(F("] G["));
	debug.print(smoke_val[G]);
	debug.print(F("]"));
	debug.println();
#endif
}

void checkGas()
{
	int reading = analogRead(GAS_PIN);
#if 1
	debug.print(F("GAS["));
	debug.print(reading);  
	debug.print(F("]"));
	debug.println();
#endif
}

void readTemp()
{
	int Vo = analogRead(THERMISTOR_PIN);  
	float R1 = 10000;
	float R2 = R1 * (1023.0 / (float)Vo - 1.0);  
	float logR2 = log(R2);  
	float c1 = 1.009249522e-03, c2 = 2.378405444e-04, c3 = 2.019202697e-07;
	float T = (1.0 / (c1 + c2*logR2 + c3*logR2*logR2*logR2));  
	float Tc = T - 273.15;  // valid value
#if 1
	debug.print(F("Temperature: "));
	debug.print(Tc);  
	debug.println(F(" C"));
	debug.println();
#endif
}
#if 0 // not used
// DUST Sensor
float get_voltage(float value)
{
	// 아날로그 값을 전압 값으로 바꿈
	float V= value * 5.0 / 1024; 
	return V;
}

float get_dust_density(float voltage)
{
	// 데이터 시트에 있는 미세 먼지 농도(ug) 공식 기준
	float dust=(voltage-no_dust) / 0.005;
	return dust;
}
#endif
void readDust()
{
	digitalWrite(DUST_EN, LOW);
	delayMicroseconds(280);
	float Vo_value = analogRead(DUST_PIN); // Read Dust Sensor
	float sensor_voltage = Vo_value*(5.0/1024); // Read Sensor Voltage
	delayMicroseconds(9680);
	digitalWrite(DUST_EN, HIGH);
	delayMicroseconds(9680);

	delay(3000);
	// 미세 먼지 밀도
	float dust_density = (0.17*sensor_voltage-0.1)*1000;  // converter voltage to dust ub/m3
#if 1
	debug.print(F("DUST(ug/m3) "));
//	debug.print(Vo_value);  
//	debug.print(F(" "));
//	debug.print(sensor_voltage);  
//	debug.print(F(" "));
	debug.print(dust_density);  // valid value
	debug.println();
#endif
}
