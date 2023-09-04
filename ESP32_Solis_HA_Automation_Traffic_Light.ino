#include <WiFi.h>
//#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP32Time.h>
#include <FastLED.h>

#define LED_PIN     12

// Information about the LED strip itself
#define NUM_LEDS    16
#define CHIPSET     WS2811
#define COLOR_ORDER GRB
CRGB leds[NUM_LEDS+1];

#define BRIGHTNESS  32


//WiFiManager wifiManager;
#define WIFI_SSID "#####################"
#define WIFI_PASSWORD "#######################"

//EEPROM Locations for values between boots
#define REBOOT_ADDR 1
#define CHARGETIME_ADDR 5
#define DAYS_ADDR  10
#define SOLARKWH_ADDR 20
#define SOLARCOST_ADDR 30
#define SOC_ADDR 40
#define CHARGEHOURS_ADDR 50
#define CHARGING_ADDR 60
#define SUNUP_ADDR 65

//******** Change these values to what you require **********
float ChargePowerLimit = 2.500; //What battery charge current have you set on the Inverter?

const float BatteryCapacitykWh = 12.0; //Total battery capacity.  Assuming 100% to 0% SoC
float PowerBeforeSun = 3.0; //No kWh used between midnight & when the panels start generating.  Phantom Power * No Hours?
float SolarDampingFactor = 0.7; //Solar forecast tends to be optimistic.  This is a scaling factor to correct
float Phantom = 0.42; //Estimate of Phantom Power load
//const int UPDATE_HOUR = 18; //Time GMT at which Solis registers update
//***************************************************************

//Solcast Solar Forecast
const char* SolarAPIKey = "#############################"; // Replace with your Solcast API key
String SolarURL = "https://api.solcast.com.au/rooftop_sites/8db5-bc30-c169-8416/forecasts?format=json";

//Octopus Agile
const char* AgileHost = "api.octopus.energy";
const char* AgileURL = "/v1/products/AGILE-18-02-21/electricity-tariffs/E-1R-AGILE-18-02-21-H/standard-unit-rates/";

//Time Server
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;
//ESP32Time rtc;
ESP32Time rtc(3600);  // offset in seconds GMT+1

//Home Assistant Integrations
#define HOME_ASSISTANT_IP "192.168.1.203"
//#define HOME_ASSISTANT_IP "https://###################" //Use Nabu Casa address for remote access
#define HOME_ASSISTANT_PORT 8123
#define HOME_ASSISTANT_TOKEN "###########################################################"


const int httpsPort = 443;
const int NUM_TARIFFS = 48;
double tariffs[NUM_TARIFFS];


double TariffMin = 100000; //Holder for the min found so far
double StartTime = 0; //No Hours after midnight to start the charge

int RebootCycle = 1;
double ChargeTime = 1;
double BatterySOC = 0;
double SolarKWh = 1;
int DaysToday = 0; //Represents the number of days since AD 0  
bool updateSolaris = false; //Send update info to Solaris.  Only happens once per day
double SunUpHour = 9; //read from Solar Forcast as first estimate above 200W
// create an HTTP client object
WiFiClient wifiClient;
HTTPClient http;

DynamicJsonDocument doc(16100);


void setup() {
  Serial.begin(115200);

  if (!EEPROM.begin(640)) {
    Serial.println("Failed to initialise EEPROM");
    Serial.println("Restarting...");
    delay(1000);
    ESP.restart();
  }  

  //**********************************************************************
  //EEPROM.writeInt(REBOOT_ADDR, 1);  //Initialise EEPROM Boot cycle (if you want to reset)
  //EEPROM.commit();
 //**********************************************************************

  //Setup LEDs for Traffic Light
  FastLED.addLeds<CHIPSET, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection( TypicalSMD5050 );
  FastLED.setBrightness( BRIGHTNESS );


  //wifiManager.autoConnect("AutoConnectAP");

  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int ctr = 0;
  int cycle = 0;
  while (WiFi.status() != WL_CONNECTED) {
    for(int i = NUM_LEDS; i>=0; i--) {
      leds[i] = 0;     
      delay(150);          
    }
    delay(200);
    Serial.print(".");
    leds[ctr] = CRGB(0,64,0);
    ctr = ctr+1;
    if(ctr>=NUM_LEDS) {
      ctr=0;
      cycle++;
    }
    if(cycle > 1) ESP.restart(); //reboot the device to try again    
    FastLED.show();
  }

  Serial.println();
  Serial.println("Connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if((EEPROM.readInt(REBOOT_ADDR)>0) && (EEPROM.readInt(REBOOT_ADDR)<5)) RebootCycle = EEPROM.readInt(REBOOT_ADDR);

  // Connect to NTP server
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  while (!time(nullptr)) {
    //Serial.println("Waiting for NTP time sync...");
    delay(1000);
  }

  //Set the RTC from NTP
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)){
    rtc.setTimeStruct(timeinfo); 
  }
  printLocalTime();  
  //This routing ensures the Inverter is only updated once per day at 23:00hrs
  //Construct a number representing todays date
/* Run as an automation instead
  DaysToday = rtc.getYear()*365 + rtc.getMonth() * 31 + rtc.getDay();
  if(DaysToday != EEPROM.readInt(DAYS_ADDR)) {
    //Routine has not been run today yet
    if(rtc.getHour(true) != UPDATE_HOUR) { 
      Serial.println("Automation has not been run yet today -");
      Serial.print("Hour now: "); Serial.print(rtc.getHour(true)); Serial.print(" - Waiting until "); Serial.print(UPDATE_HOUR); Serial.println(":00 to update Solis Registers");        
    } else {
      if(RebootCycle == 4){
        //All the cycles have been run.  Set Days to today so I won't run again until tomorrow
        EEPROM.writeInt(DAYS_ADDR,DaysToday); //Set last run date
        EEPROM.commit();
        updateSolaris = true;  //Set flag to update on this run
        Serial.println("************************  Updating Solis on this cycle  ************************");
      }
    }
  }

*/


  LightMeUp(EEPROM.readInt(CHARGING_ADDR)); //Show LED Traffic Light

   if(RebootCycle==1) {//Get HomeAssistant Battery SoC
    //Serial.println("**** Get SOC Data **** ");
    BatterySOC = getHABatterySOC();
    EEPROM.writeInt(CHARGING_ADDR, getHACharging());  //Charging True if power into battery > 0

    if(BatterySOC !=0) {
      RebootCycle = 2;
      //leds[0] = leds[0] + CRGB(0,0,64);
      FastLED.show();
      EEPROM.writeInt(REBOOT_ADDR, RebootCycle); //4 Bytes for Int - Get Solar Forcast on next reboot
      EEPROM.writeDouble(SOC_ADDR, BatterySOC); //8 Bytes for a Double - Store the Start Time
      EEPROM.commit();
      Serial.println("#################################################################");
      Serial.print("Reboot Cycle: "); Serial.println(1);
      Serial.print("Battery SoC: "); Serial.println(EEPROM.readDouble(SOC_ADDR));
      Serial.println("#################################################################");
      Serial.println("Restarting in 30 sec");
      delay(30000);
      ESP.restart(); //reboot the device
    } else {
      Serial.println("Restarting in 10 sec  Reboot Cycle 1");
      delay(10000);
      ESP.restart(); //reboot the device to try again
    }
   
  }
  if(RebootCycle==2) { //Get Solar Data
    
    Serial.print("Hour:Minute ");Serial.print(rtc.getHour()); Serial.print(":"); Serial.println(rtc.getMinute());
//    if((rtc.getHour() == UPDATE_HOUR) && (rtc.getMinute() < 15))  { 
//      Serial.println("**** Get Solcast.au Forcast **** ");
//      SolarKWh = Get_Solar_Forcast(); //Get form SolCast Can only get 5 per day for free
      
//    } else {
      Serial.println("**** Get HA Solar Forcast **** ");
      SolarKWh = getHASolarkWh(); //Get from HA
//    }
    SolarDampingFactor = GetHADamping();
    SolarKWh = SolarKWh * SolarDampingFactor; //Forecasts rather optimistic. Fix that!

    if(SolarKWh !=0) {
      RebootCycle = 3;
      //leds[1] = leds[1] + CRGB(0,0,64); 
      FastLED.show();
      EEPROM.writeInt(REBOOT_ADDR, RebootCycle); //Get Print the results & wait 24h
      EEPROM.writeInt(SUNUP_ADDR, SunUpHour); //Get Print the results & wait 24h
      EEPROM.writeDouble(SOLARKWH_ADDR, SolarKWh); //8 Bytes for a Double - Store the Start Time
      EEPROM.commit();
      Serial.println("#################################################################");
      Serial.print("Reboot Cycle: "); Serial.println(2);
      Serial.print("SolarKWh: "); Serial.println(EEPROM.readDouble(SOLARKWH_ADDR));
      Serial.println("#################################################################");
      Serial.println("Restarting in 30 sec");
      delay(30000);
      ESP.restart(); //reboot the device
    } else {
      Serial.println("Restarting in 10 sec  Reboot Cycle 2");
      delay(10000);
      ESP.restart(); //reboot the device to try again
    }
  }

  if(RebootCycle==3) {//Get Agile Data
    //Serial.println("**** Get Agile Data **** ");
    //Calculate how much charge needed
    float ChargeRequiredkWh = 0; //How much charge in kWh do you want to put into the battery tonight?
    double BatSOC = EEPROM.readDouble(SOC_ADDR);
    double SolKWH = EEPROM.readDouble(SOLARKWH_ADDR);
    double BatCharge = (BatSOC - 20) * BatteryCapacitykWh / 100;
    //Set PowerBeforeSun from Phantom Load x Hours before sunrise.  This will be most accurate close to midnight
    PowerBeforeSun = EEPROM.readInt(SUNUP_ADDR) * GetHAPhantom();
  
    double MinimumBatteryCharge = PowerBeforeSun - BatCharge;  //This is the amount of power needed before batteries start charging from Solar
    //If MinimumBatteryCharge is -ve, that's the battery charge remaining in the morning
     
    ChargeRequiredkWh = BatteryCapacitykWh -SolKWH; //This is the amount of charge we need to put into the battery overnight
    if(ChargeRequiredkWh<0) ChargeRequiredkWh = 0; //Can't put in less than none
    ChargeRequiredkWh = ChargeRequiredkWh + MinimumBatteryCharge;
    if(ChargeRequiredkWh>BatteryCapacitykWh) ChargeRequiredkWh=BatteryCapacitykWh;

    Serial.print("SolKWH = "); Serial.println(SolKWH);
    Serial.print("BatCharge = "); Serial.println(BatCharge);
    Serial.print("SunUpHour = "); Serial.println(SunUpHour);
    Serial.print("PowerBeforeSun = "); Serial.println(PowerBeforeSun);
    
    Serial.print("MinimumBatteryCharge = PowerBeforeSun - BatCharge = "); Serial.println(MinimumBatteryCharge);
    Serial.print("ChargeRequiredkWh = BatteryCapacitykWh + MinimumBatteryCharge -SolKWH = "); Serial.println(ChargeRequiredkWh);
    
    ChargePowerLimit = getChargeCurrentLimit() * 0.048; //Battery voltage * Current to give Power
    Serial.print("ChargePowerLimit = "); Serial.println(ChargePowerLimit);
    
    float ChargeHours = (2.0* ChargeRequiredkWh / ChargePowerLimit); //No Half Hours to charge the battery to that level.  Add 0.5 to round properly
    Serial.print("ChargeHours = 2.0* ChargeRequiredkWh / ChargePowerLimit = "); Serial.println(ChargeHours);
    if(ChargeRequiredkWh>0) {
      ChargeTime = Get_Cheapest_Agile_Period(ChargeHours, ChargeRequiredkWh, ChargePowerLimit);
      
      EEPROM.writeDouble(CHARGETIME_ADDR, ChargeTime); //8 Bytes for a Double - Store the Start Time
      EEPROM.writeDouble(CHARGEHOURS_ADDR, ChargeHours); //8 Bytes for a Double - Store the Start Time
      EEPROM.commit();
      //leds[0]= leds[0] + CRGB(0,0,64); leds[1] = leds[1] + CRGB(0,0,64); 
      if(ChargeTime !=0) {
        
         FastLED.show();
        Serial.println("#################################################################");
        Serial.print("Reboot Cycle: "); Serial.println(EEPROM.readInt(REBOOT_ADDR));
        Serial.print("Charge Start Time: "); Serial.println(EEPROM.readDouble(CHARGETIME_ADDR));
        Serial.print("Charge Duration Hours: "); Serial.println(EEPROM.readDouble(CHARGEHOURS_ADDR));
        Serial.print("Battery Charging Cost: "); Serial.println(EEPROM.readDouble(SOLARCOST_ADDR));
        Serial.println("#################################################################");
        RebootCycle = 4;
        EEPROM.writeInt(REBOOT_ADDR, RebootCycle); //4 Bytes for Int - Get Solar Forcast on next reboot
        EEPROM.commit();        
        Serial.println("Restarting in 30 sec");
        delay(30000);
        ESP.restart(); //reboot the device
      } else {
        Serial.println("Restarting in 10 sec  Reboot Cycle 3 Agile");
        delay(10000);
        ESP.restart(); //reboot the device to try again
      }
    } else {
      ChargeTime = 0;
      Serial.println("#################################################################");
      Serial.print("Reboot Cycle: "); Serial.println(EEPROM.readInt(REBOOT_ADDR));
      Serial.println("** NO CHARGE REQUIRED **");
      Serial.println("#################################################################");
      EEPROM.writeInt(REBOOT_ADDR, 4); //4 Bytes for Int - Get Solar Forcast on next reboot
      EEPROM.writeDouble(CHARGETIME_ADDR, 0); //8 Bytes for a Double - Store the Start Time
      EEPROM.writeDouble(CHARGEHOURS_ADDR, 0); //8 Bytes for a Double - Store the Start Time
      EEPROM.commit();
      Serial.println("Restarting in 30 sec");
      delay(30000);
      ESP.restart(); //reboot the device
      
    }
  }

  if(RebootCycle==4) { //Print results, wait 70 mins then set an alarm for 23:00 tomorrow
    
    Serial.println("#################################################################");
    Serial.print("Reboot Cycle: "); Serial.println(EEPROM.readInt(REBOOT_ADDR));
    Serial.print("Battery SoC: "); Serial.println(EEPROM.readDouble(SOC_ADDR));
    Serial.print("ChargeTime: "); Serial.println(EEPROM.readDouble(CHARGETIME_ADDR));
    Serial.print("ChargeHours: "); Serial.println(EEPROM.readDouble(CHARGEHOURS_ADDR));
    Serial.print("SolarKWh: "); Serial.println(EEPROM.readDouble(SOLARKWH_ADDR));
    Serial.print("BatteryChargingCost: "); Serial.println(EEPROM.readDouble(SOLARCOST_ADDR));

    //Now we have all the data, calculate how much to charge the battery tonight & set values in HA
    double CT = EEPROM.readDouble(CHARGETIME_ADDR);
    double CH = EEPROM.readDouble(CHARGEHOURS_ADDR);
    
    int Start_H = CT; //Integer part of hour
    int Start_M = int((CT - float(Start_H)) * 60);
    int End_H = Start_H + int(CH);
    
    int End_M = Start_M + (CH - int(CH))*60; 
    if(End_M > 59) {
      End_M = End_M - 60;
      End_H = End_H + 1;
    }   
    if(End_H==24) End_H = 0; //24 O'Clock isn't allowed!
    Serial.print("Charge Start: "); Serial.print(Start_H); Serial.print(":"); Serial.println(Start_M);
    Serial.print("Charge End: "); Serial.print(End_H); Serial.print(":"); Serial.println(End_M);
    Serial.println("#################################################################");

    Serial.println("Sending data to Home Assistant");  

    
    Serial.println("Update Home Assistant Charge Start & End");  
    //Update charge start & end times
    callHomeAssistantService("number", "set_value", "number.solismodbus_timed_charge_start_hours", Start_H);
    callHomeAssistantService("number", "set_value", "number.solismodbus_timed_charge_start_minutes", Start_M);
    callHomeAssistantService("number", "set_value", "number.solismodbus_timed_charge_end_hours", End_H);
    callHomeAssistantService("number", "set_value", "number.solismodbus_timed_charge_end_minutes", End_M);
    //Update charge current
    //callHomeAssistantService("number", "set_value", "number.solismodbus_timed_charge_current", ChargePowerLimit);
  
  /*  Run as an automation instead
  if(updateSolaris){  
    //Committ changes
    Serial.println("************************  Send HA Data to Inverter  ************************");
  for(int i = NUM_LEDS; i>=0; i--) {
    leds[i] = CRGB(0,0,64); //Turn all the LEDs Blue for this cycle
    FastLED.show();     
    delay(50);
         
  }
    callHomeAssistantService("button", "turn_on", "button.solismodbus_update_charge_discharge_times", 0);
  }
*/
  //leds[0] = 0; leds[1] = 0; 
  FastLED.show();

    delay(60000);
    RebootCycle = 1;
    EEPROM.writeInt(REBOOT_ADDR, RebootCycle); //Get Agile Data
    EEPROM.commit();
    ESP.restart(); //reboot the device
  }
}


void loop() {
  // do nothing
}


void LightMeUp(int Charging) {
//Use the FastLED Library to show a traffic light indicating whether it's a good time to use loads of power!
  //First fade out the LEDs
  for(int i = NUM_LEDS; i>=0; i--) {
    leds[i] = 0;
    FastLED.show();     
    delay(150);
         
  }

  //Set new pattern
  int TrafficLight = (EEPROM.readDouble(SOC_ADDR)-20)/80 * (NUM_LEDS);
  int ChargingLight = Charging/1000; 
  //if(Charging) TrafficLight = (EEPROM.readDouble(SOC_ADDR)-10)/80 * (NUM_LEDS-1); //Add 10% to display if charging
  

  bool ShowBlue = false;
  if(TrafficLight>(NUM_LEDS)) {
    TrafficLight = NUM_LEDS;  
  }
  if(TrafficLight<0) {
    TrafficLight = 0;  
  }
  
  //Serial.print("### TRAFFIC LIGHTS ###  "); Serial.print(TrafficLight); Serial.print(" ChargingLight "); Serial.println(ChargingLight);

  //Show a blue light at top if it's a REALLY good time to use power  
  //if(Charging && (EEPROM.readDouble(SOC_ADDR)>95)) ShowBlue = true;

  int First = TrafficLight + ChargingLight - 1;
  if(First>=NUM_LEDS) First = NUM_LEDS;
  int Last = -1;
  int Inc =  -1;  
  
  if(Charging>=0){ //Battery is charging, reverse lights
    First = 0;
    Last = TrafficLight+ChargingLight;
    if(Last>=NUM_LEDS) Last = NUM_LEDS;
    Inc =  +1;  
  }

  for(int k = First; k!= Last; k = k+Inc) {
    if(k<=TrafficLight){
      if(k<(NUM_LEDS/3)) leds[k] = CRGB(64,0,0); //Set to Red
      if((k>=(NUM_LEDS/3)) && (k<=(NUM_LEDS*2/3))) leds[k] = CRGB(64,32,0); //Set to Orange
      if((k>(NUM_LEDS*2/3)) && (k<=(NUM_LEDS))) leds[k] = CRGB(0,64,0); //Set to Green
      if(ShowBlue) leds[NUM_LEDS-1] = CRGB(0,0,64); //If charging and battery full, make top LED Blue
    } else {
      leds[k] = CRGB(16,16,12); //Set to dim white      
    }
    FastLED.show(); 
    delay(150);        
  } 
  
}

String callHomeAssistantService(const char *domain, const char *service, const char *entity_id, int value) {
  String data = String("{\"entity_id\":\"") + entity_id + "\",\"value\":" + value + "}";
  
  //httpClient.begin(wifiClient, String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/services/" + domain + "/" + service);
  http.begin(String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/services/" + domain + "/" + service);
  http.addHeader("Authorization", String("Bearer ") + HOME_ASSISTANT_TOKEN);
  http.addHeader("Content-Type", "application/json");

  int httpResponseCode = http.POST(data);
  String response = http.getString();
  http.end();

  if (httpResponseCode != 200) {
    Serial.printf("Error %d: %s\n", httpResponseCode, response.c_str());
  }

  return response;
}

float getChargeCurrentLimit() {
  return 52;

/*
  http.begin(String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/states/input_number.set_overnight_charge_current");
  http.addHeader("Authorization", String("Bearer ") + HOME_ASSISTANT_TOKEN);

  int httpResponseCode = http.GET();
  String response = http.getString();
  http.end();

  if (httpResponseCode != 200) {
    Serial.printf("Error %d: %s\n", httpResponseCode, response.c_str());
    return 0.0;
  }

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, response);
  float ChargePower = doc["state"].as<float>();

  Serial.print("Overnight Charge Limit = "); Serial.println(ChargePower);
  return ChargePower * 48; //Convert to current
*/
 
}
float getHABatterySOC() {
  http.begin(String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/states/sensor.solismodbus_battery_soc");
  http.addHeader("Authorization", String("Bearer ") + HOME_ASSISTANT_TOKEN);

  int httpResponseCode = http.GET();
  String response = http.getString();
  //Serial.println(String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/states/sensor.solismodbus_battery_soc");
  http.end();

  if (httpResponseCode != 200) {
    Serial.printf("Error %d: %s\n", httpResponseCode, response.c_str());
    return 0.0;
  }

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, response);
  float batterySoc = doc["state"].as<float>();

  return batterySoc;
}

float getHACharging() {
  http.begin(String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/states/sensor.solismodbus_battery_input_energy");
  http.addHeader("Authorization", String("Bearer ") + HOME_ASSISTANT_TOKEN);

  int httpResponseCode = http.GET();
  String response = http.getString();
  //Serial.println(String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/states/sensor.solismodbus_battery_soc");
  http.end();

  if (httpResponseCode != 200) {
    Serial.printf("getHACharging Error %d: %s\n", httpResponseCode, response.c_str());
    return 0.0;
  }

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, response);
  float BattChargeWatts = doc["state"].as<float>();
  Serial.print("BattChargeWatts = "); Serial.println(BattChargeWatts);
  return BattChargeWatts;
}




float GetHAPhantom() {
  return Phantom;

/*  
  http.begin(String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/states/input_number.phantom_load_w");
  http.addHeader("Authorization", String("Bearer ") + HOME_ASSISTANT_TOKEN);

  int httpResponseCode = http.GET();
  String response = http.getString();
  //Serial.print("Phantom "); Serial.println(response);
  

  if (httpResponseCode != 200) {
    Serial.print("Phantom ");
    Serial.printf("Error %d: %s\n", httpResponseCode, response.c_str());
    http.end();
    return 0.35; //The phantom power is about 0.35kWh
  }

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, response);
  float Phantom = doc["state"].as<float>();
  Serial.print("Phantom Power = "); Serial.println(Phantom);
  http.end();
  return Phantom;
*/
}


float GetHADamping() {
  //return Phantom;

  
  http.begin(String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/states/input_number.solar_damping_factor");
  http.addHeader("Authorization", String("Bearer ") + HOME_ASSISTANT_TOKEN);

  int httpResponseCode = http.GET();
  String response = http.getString();
  //Serial.print("Damping "); Serial.println(response);
  http.end();

  if (httpResponseCode != 200) {
    Serial.print("Damping ");
    Serial.printf("Error %d: %s\n", httpResponseCode, response.c_str());
    return 0.9; //The typical Damping factor is about 0.9
  }

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, response);
  float Damping = doc["state"].as<float>();
  Serial.print("Damping Factor = "); Serial.println(Damping);

  return Damping;

}



float getHASolarkWh() {
  http.begin(String("http://") + HOME_ASSISTANT_IP + ":" + HOME_ASSISTANT_PORT + "/api/states/sensor.energy_production_tomorrow_3");
  http.addHeader("Authorization", String("Bearer ") + HOME_ASSISTANT_TOKEN);

  int httpResponseCode = http.GET();
  String response = http.getString();
  //Serial.println("********************************************");
  //Serial.println(response);
  //Serial.println("********************************************");
  

  http.end();

  if (httpResponseCode != 200) {
    Serial.printf("Error %d: %s\n", httpResponseCode, response.c_str());
    return 0.0;
  }

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, response);
  float solarkWh = doc["state"].as<float>();

  return solarkWh;
}





void printLocalTime()
{
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.print("Current time: ");
  Serial.print(asctime(&timeinfo));
}


double Get_Solar_Forcast(){
//Get the forcast No kWh of sunlight for tomorrow
double dTomorrowTotal = 0.0;

  http.begin(SolarURL);
  http.addHeader("Accept-Encoding", "gzip, deflate, br");
  http.addHeader("Authorization", "Bearer " + String(SolarAPIKey));  
 
  int httpCode = http.GET();
  //Serial.print("HTTP response code: ");
  //Serial.println(httpCode);
  
  if (httpCode > 0) {

    
    String payload = http.getString();
    //Serial.println("Solar payload:");
    //Serial.println(payload);
    
    DeserializationError error = deserializeJson(doc, payload);    
    if (error) {
      Serial.print("deserializeJson() failed: ");
      Serial.println(error.c_str());
      //Serial.println(SolarURL);
      return 0;
    }
    int count=0;
    bool bTomorrow = false;
    SunUpHour = 23;

    int nCount = 0;
    for (JsonObject forecast : doc["forecasts"].as<JsonArray>()) {
    
      //float pv_estimate = forecast["pv_estimate"]; // 3.8556, 3.6041, 3.281, 2.9187, 2.4915, 1.9523, ...
      String period_end = forecast["period_end"]; // "2023-04-03T14:00:00.0000000Z", ...

      //Other unused fields that are available
      //float forecast_pv_estimate10 = forecast["pv_estimate10"]; // 3.8556, 3.6041, 3.281, 2.9187, 2.4564, ...
      //float forecast_pv_estimate90 = forecast["pv_estimate90"]; // 3.8556, 3.6041, 3.281, 2.9187, 2.4915, ...
      //const char* period_end = forcast["period_end"];
      //const char* date_str = strstr(period_end, "T"); // get the date part


      int splitT = period_end.indexOf("T");
      int posHour = period_end.indexOf(":");
      String timeStamp = period_end.substring(splitT, period_end.length()-1);  //Time without preceeding T
      
      String sHour = timeStamp.substring(1, 3);
      String sMinute = timeStamp.substring(4, 6);
      String pv_estimate = forecast["pv_estimate"];
      
      int nHour = sHour.toInt();
      int nMinute = sMinute.toInt();
      float dEstimate = pv_estimate.toDouble();
      
      //Serial.print("JSON Hour: "); Serial.print(nHour);
      //Serial.print("   Minute: "); Serial.print(nMinute);
      int nTMins = nHour*60+nMinute;  
      if(nTMins < 30) bTomorrow = true; //Crossed midnight, get thext 24h of data
      
      if(bTomorrow && (nCount < 48)) {    
        //There are 48 readings in 24h - so once the first midnight is detected, get the next 48 readings 
        nCount++; 
        dTomorrowTotal = dTomorrowTotal + dEstimate;
        if(dEstimate>(GetHAPhantom())) {
          //This is effectively the time of sun-up
          if(nHour < SunUpHour) {
            SunUpHour = nHour;
            Serial.print("Sun Up Tomorrow = "); Serial.println(SunUpHour);
          }
        }
        //Serial.print("Time: "); Serial.print(nHour); Serial.print(":");Serial.print(nMinute);
        //Serial.print("  Half Hour Estimate: "); Serial.print(dEstimate);
        //Serial.print("    Cumulative Total "); Serial.print(dTomorrowTotal); Serial.print("kWh"); Serial.println();
      }
   
    }
    dTomorrowTotal = dTomorrowTotal / 2; //Total is the sum of half hours, in kW per hour so need to /2 for actual kWh
    doc.garbageCollect();
    doc.clear();
    //Serial.print("Cumulative Total "); Serial.print(dTomorrowTotal); Serial.print("kWh"); Serial.println();

  } else {
    Serial.println("HTTP request failed");
    return 0;
  }

  http.end();
  return dTomorrowTotal; //Figure adjusted for forecast optimism
}






double Get_Cheapest_Agile_Period(double PeriodHours, double ChargekWh, double ChargePower){
  // set the target URL
  String AgileResponse;
  http.end();
  http.begin("https://" + String(AgileHost) + String(AgileURL));
  http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:109.0) Gecko/20100101 Firefox/112.0");
 
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-type", "application/json");
  // send a GET request to the Octopus API endpoint
  int httpResponseCode = http.GET();

  if (httpResponseCode == HTTP_CODE_OK) {
    // read the server's response
    //DynamicJsonDocument jsonBuffer(12000);
    AgileResponse = http.getString();
    DeserializationError error = deserializeJson(doc, AgileResponse);    

    if (error) {
      Serial.print("deserializeJson() failed: ");
      Serial.println(error.c_str());
      Serial.println(AgileResponse);
      Serial.println("https://" + String(AgileHost) + String(AgileURL));
      return 0;
    }
    int nCount=0;
    bool bTomorrow = false;
    
    // extract the tariff values from the JSON response and store them in the tariffs array
    for (JsonObject forecast : doc["results"].as<JsonArray>()) {
      double valueIncVat = forecast["value_inc_vat"]; //Value in Pence
      valueIncVat = valueIncVat / 100; //Value in £
      String period_end = forecast["valid_from"]; // "2023-04-03T14:00:00.0000000Z", ...
      

      int splitT = period_end.indexOf("T");
      int posHour = period_end.indexOf(":");
      String timeStamp = period_end.substring(splitT, period_end.length()-1);  //Time without preceeding T
      
      String sHour = timeStamp.substring(1, 3);
      String sMinute = timeStamp.substring(4, 6);
      
      int nHour = sHour.toInt();
      int nMinute = sMinute.toInt();

      int nTMins = nHour*60+nMinute;  
      if(nTMins < 30) bTomorrow = true; //Crossed midnight, get thext 24h of data
      
      if(bTomorrow && (nCount < 48)) {    
        //There are 48 readings in 24h - so once the first midnight is detected, get the next 48 readings 
        nCount++; 
        tariffs[nCount] = valueIncVat;

        
        //Serial.print("Time: "); Serial.print(nHour); Serial.print(":");Serial.print(nMinute);
        //Serial.print("  Half Hour Tariff: "); Serial.println(valueIncVat);
      }

    }
    doc.garbageCollect();
    doc.clear();
    
    //Serial.print("ChargeHours "); Serial.println(ChargeHours);
    int count = 47;
    while(count > -1) {
      //Tariffs in half hour slots - so multiply Charge hours by 2
      double TariffTotal = 0;
      for (int Interval = PeriodHours; Interval > 0 ; Interval--) {
        int nHalfHour = (count - Interval);
        if(nHalfHour<0) nHalfHour = 48+nHalfHour;
        TariffTotal = TariffTotal + tariffs[nHalfHour]; //Sum the tariffs for this period. % (Modulo) just wraps around at midnight
      }
      if(TariffTotal<TariffMin){
        TariffMin = TariffTotal;
        StartTime = count/2.0;
      }
      count--;
      if(count<0) break;
    }
    double ChargeCost = TariffMin / 2 * ChargePower;
    //Serial.print("Start the charge at: ");Serial.print(StartTime);Serial.print(" Hours past midnight for: ");Serial.print(PeriodHours);Serial.println(" Hours");
    //Serial.print("Charge will cost: £");Serial.println(ChargeCost); //TariffMin is the sum of the rates over the period (in half hours) * the number of kW per hour delivered
    EEPROM.writeDouble(SOLARCOST_ADDR, ChargeCost);
    EEPROM.commit();
  } else {
    Serial.println("Agile HTTP GET request failed!");
    Serial.println("https://" + String(AgileHost) + String(AgileURL));
    Serial.print("Response Code: "); Serial.println(httpResponseCode);
    return 0;
  }

  // close the HTTP connection
  http.end();
  return StartTime;

}
