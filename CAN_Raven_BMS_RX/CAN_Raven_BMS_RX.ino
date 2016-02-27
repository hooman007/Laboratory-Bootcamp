//Code by Patrick Cruce(pcruce_at_igpp.ucla.edu) 
//Uses CAN extended library, designed for Arduino with 16 Mhz oscillator
//Library found at: http://code.google.com/p/canduino/source/browse/trunk/#trunk%2FLibrary%2FCAN
//This runs a simple test of the hardware with the MCP 2515 CAN Controller in loopback mode.
//If using over physical bus rather than loopback, and you have high bus 
//utilization, you'll want to turn the baud of the serial port up or log
//to SD card, because frame drops can occur due to the reader code being
//occupied with writing to the port.
//In our testing over a 40 foot cable, we didn't have any problems with
//synchronization or frame drops due to SPI,controller, or propagation delays
//even at 1 Megabit.  But we didn't do any tests that required arbitration
//with multiple nodes.

//ZEVA PROTOCOL AT http://zeva.com.au/Products/datasheets/BMS12_CAN_Protocol.pdf

#include <mcp_can.h>
#include <SoftwareSerial.h>
#include <SPI.h>

#include "bms_defs.h"
#include <ubcsolar_can_ids.h>

#define DEBUG 0 //print contents of all packets received instead of once per second

#define CAN_SS 9

#define BUS_SPEED CAN_125KBPS
#define PRINT_DELAY 1000
#define DIAG_PRINT_DELAY 250

#define BINMSG_SEPARATOR 0xFF
#define BINMSG_MAXVAL 0xFE

typedef enum{
  DIAG_OFF,
  DIAG_BMS_CORE,
  DIAG_BMS_CELLS,
  DIAG_OTHERS
} DiagMode;

MCP_CAN CAN(CAN_SS);

BMSConfig bmsConfig = {0}; //set valid to 0
BMSStatus bmsStatus = {0};
int cellVoltagesX100[4][12] = {{0}};
signed char bmsTemperatures[4][2] = {{0}};

unsigned char bmsAlive = 0;
unsigned long int lastPrintTime = 0;

byte target_throttle = 0;
byte target_regen = 0;
byte target_dir = 0;
unsigned long int lastMotorCtrlRxTime = 0;
byte brake_on = 0;

DiagMode diagnosticMode = DIAG_OFF;

void setup() {  
/* SERIAL INIT */
  Serial.begin(115200);

/* CAN INIT */
  int canSSOffset = 0;

CAN_INIT:

  if(CAN_OK == CAN.begin(BUS_SPEED))                   // init can bus : baudrate = 125k
  {
    Serial.println("CAN BUS Shield init ok!");
  }
  else
  {
    Serial.println("CAN BUS Shield init fail");
    Serial.print("Init CAN BUS Shield again with SS pin ");
    Serial.println(CAN_SS + canSSOffset);
    delay(100);
    canSSOffset ^= 1;
    CAN = MCP_CAN(CAN_SS + canSSOffset);
    goto CAN_INIT;
  }
}

void printBuf(uint32_t frame_id, byte *frame_data, byte length) {
     
  Serial.print("[Rx] ID: ");
  Serial.print(frame_id,HEX);
        
  Serial.print(" Len:");
  Serial.print(length,HEX);
      
  Serial.print(" Data:[");
  for (int i=0;i<length;i++) {
    if (i>0) Serial.print(" ");
    Serial.print(frame_data[i],HEX);
  }
  Serial.println("]"); 
}

void msgHandleZevaBms(uint32_t frame_id, byte *frame_data, byte length) {
  
  uint32_t messageID = frame_id%10;
  
  // even IDs are requests from Core to BMS12
  if(messageID%2 == 0) {
    #if DEBUG
    switch (messageID) {
      case 0: Serial.print("Request for status from Core to Module "); break;
      case 2: Serial.print("Request for voltages #1 from Core to Module "); break;
      case 4: Serial.print("Request for voltages #2 from Core to Module "); break;
      case 6: Serial.print("Request for config info from Core to Module "); break;
      case 8: Serial.print("Command to set config info from Core to Module "); break;
      default: Serial.print("Unknown message from Core to Module "); break;
    }
    Serial.print((frame_id - 100 - messageID)/10);  // frameID = 100 + 10 * moduleID + messageID, 0 <= messageID <=8
    Serial.println();
    #endif
    return;
  }
  
  if(messageID != 3 && messageID != 5){
    Serial.print("BMS #");
    Serial.print((frame_id-100)/10);
    Serial.print(" packet ");
    Serial.print(frame_id%10);
    Serial.println(" parsing not implemented");
    return;
  }
  
  byte bmsId=(frame_id-100)/10;
  byte voltGrp=(frame_id%10)/2-1;
  
  for(int i=0; i<6; i++) {
    cellVoltagesX100[bmsId][voltGrp*6+i] = frame_data[i]+((frame_data[6]>>i)&1 ? 256 : 0);
  }
  bmsTemperatures[bmsId][voltGrp] = frame_data[7] - 128;
  
  #if DEBUG
  Serial.print("BMS #");
  Serial.print(bmsId);
  for(int i=0; i<6; i++) {
    Serial.print(" c");
    Serial.print(i+6*voltGrp);
    Serial.print("=");
    Serial.print(cellVoltagesX100[bmsId][voltGrp*6+i]);
  }
  Serial.print(" t");
  Serial.print(voltGrp);
  Serial.print("=");
  Serial.print(bmsTemperatures[bmsId][voltGrp]);
  Serial.println();
  #endif
}

void msgHandleMotorCtrl(uint32_t frame_id, byte *frame_data, byte length){
  target_throttle = frame_data[0];
  target_regen = frame_data[1];
  target_dir = frame_data[2];
  lastMotorCtrlRxTime = millis();
  Serial.print(frame_data[0]);
  Serial.print(" ");
  Serial.print(frame_data[1]);
  Serial.print(" ");
  Serial.println(frame_data[2]);
}

void msgHandleBrake(uint32_t frame_id, byte *frame_data, byte length){
  brake_on = frame_data[0];
}

void msgHandler(uint32_t frame_id, byte *frame_data, byte length) {
   
  if(frame_id>=CAN_ID_ZEVA_BMS_BASE && frame_id<CAN_ID_ZEVA_BMS_BASE+40) {
    msgHandleZevaBms(frame_id, frame_data, length);
  }
  else if(frame_id == CAN_ID_ZEVA_BMS_CORE_STATUS) {
    msgHandleZevaCoreStatus(frame_id, frame_data, length);
  }
  else if(frame_id >= 12 && frame_id <= 17) {
    msgHandleZevaCoreConfig(frame_id, frame_data, length);
  }
  else if(frame_id == CAN_ID_MOTOR_CTRL){
    msgHandleMotorCtrl(frame_id, frame_data, length);
  }
  else if(frame_id == CAN_ID_BRAKE){
    msgHandleBrake(frame_id, frame_data, length);
  }
  else {
    #if !DEBUG
    if(diagnosticMode != DIAG_OFF){
      diag_cursorPosition(19,1);
    #endif
    Serial.print("unknown msg ");
    printBuf(frame_id, frame_data, length);
    #if !DEBUG
    }
    #endif
  }
}

void printBinMsg(){
  unsigned char tmp;
  unsigned char checksum = 0;
  
  Serial.write(BINMSG_SEPARATOR); //print twice incase one gets lost
  Serial.write(BINMSG_SEPARATOR);

  tmp = 0; //TODO: speed
  Serial.write(tmp);
  checksum += tmp;
  
  tmp = (bmsStatus.voltage < 0 ? 0 : (bmsStatus.voltage > BINMSG_MAXVAL ? BINMSG_MAXVAL : bmsStatus.voltage));
  Serial.write(tmp);
  checksum += tmp;
  
  tmp = (bmsStatus.soc < 0 ? 0 : (bmsStatus.soc > BINMSG_MAXVAL ? BINMSG_MAXVAL : bmsStatus.soc));
  Serial.write(tmp);
  checksum += tmp;
  
  tmp = (bmsStatus.temperature < 0 ? 0 : (bmsStatus.temperature > BINMSG_MAXVAL ? BINMSG_MAXVAL : bmsStatus.temperature));
  Serial.write(tmp);
  checksum += tmp;

  tmp = 1; //TODO: motor temp
  Serial.write(tmp);
  checksum += tmp;

  for(int i=0; i<4; i++){
    tmp = (bmsTemperatures[i][0] < 0 ? 0 : (bmsTemperatures[i][0] > BINMSG_MAXVAL ? BINMSG_MAXVAL : bmsTemperatures[i][0]));
    Serial.write(tmp);
    checksum += tmp;
  }

  for(int i=0; i<4; i++){
    for(int j=0; j<12; j++){
      tmp = cellVoltagesX100[i][j]/2;
      tmp = (tmp < 0 ? 0 : (tmp > BINMSG_MAXVAL ? BINMSG_MAXVAL : tmp));
      Serial.write(tmp);
      checksum += tmp;
    }
  }

  Serial.write(checksum ^ 0xFF);

  Serial.write(BINMSG_SEPARATOR);
  Serial.write(BINMSG_SEPARATOR);
}

void loop() {
  
  byte length;
  uint32_t frame_id;
  byte frame_data[8];

  if(CAN_MSGAVAIL == CAN.checkReceive()){
    CAN.readMsgBuf(&length, frame_data);
    frame_id = CAN.getCanId();

    msgHandler(frame_id, frame_data, length);
  }
  
  if(bmsAlive == 1){
    zevaCoreSetCellNum();
    bmsAlive |= 2;
  }
  
#if !DEBUG
  if(Serial.available())
    diag_getCmd(Serial.read());

  if(diagnosticMode == DIAG_OFF){
    if(millis() - lastPrintTime > PRINT_DELAY){
      lastPrintTime += PRINT_DELAY;
      printBinMsg();
    }
  }else{
    if(millis() - lastPrintTime > DIAG_PRINT_DELAY){
      lastPrintTime += DIAG_PRINT_DELAY;
      switch(diagnosticMode){
        case DIAG_BMS_CORE:
          diag_BMSCore();
          break;
        case DIAG_BMS_CELLS:
          diag_BMSCells();
          break;
        case DIAG_OTHERS:
          diag_others();
          break;
        default:
          diagnosticMode = DIAG_OFF;
          break;
      }
    }
  }
#endif
}

