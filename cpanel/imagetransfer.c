#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>

#include "defines.h"
#include "ui-menu.h"
#include "asm.h"

//
//Defined in ui.c
extern uint8_t curPos;
extern uint8_t key;
extern char numberInputBuffer[];
extern uint16_t enteredNumber;


extern const char strOKAnyKey[];


//Global variables for passing parameter to .asm and then megaflash
uint8_t tftp_dir;
uint8_t tftp_unitNum;
char tftp_hostname[41];
char tftp_filename[41];

//Function Prototypes
void StartTransfer();

void DoImageTransfer(){
  static_local bool enter;           //Enter key pressed
  static_local char key;
  
  //Ask Send or Receive
  clrscr();
  printf("TFTP Disk Image Transfer\n");
  
  //Ask for direction
  do {
  gotoxy(0,3);
  printf("1)Download from server\n2)Upload to server\n");
  printf("Please enter(1-2):");
  curPos = 0;   //No default value
  enter = EnterNumber(1,1,2);
  }while (!enter);
  tftp_dir = (uint8_t)enteredNumber-1;
  
  //Ask Unit Number
  do {
  gotoxy(0,7);
  printf("Enter Drive Number (1-4):");
  curPos = 0;  
  enter = EnterNumber(1,1,4);
  }while (!enter);
  tftp_unitNum = (uint8_t)enteredNumber;
  
  //Ask hostname:
  do{
    gotoxy(0,9);
    printf("Enter IP Addr or hostname or server");
    enter = EnterText(tftp_hostname,40,0,10,40);
  }while (!enter || strlen(tftp_hostname)==0);
  
   //Ask filename:
  do{
    gotoxy(0,12);
    printf("Enter filename:");
    enter = EnterText(tftp_filename,40,0,13,40);
  }while (!enter || strlen(tftp_filename)==0); 
  
  //Confirm
again:
  gotoxy(0,15);
  printf("Confirm (Y/N):");
  key = cgetchar(' ');
  if (key=='N' || key=='n') return; //Do nothing
  if (key!='Y' && key!='y') {
    beep();
    goto again;
  }
  
  StartTransfer();
  
  printf("\n\nPress any key to continue");
  cgetc();
}

const char *STATUSMSG[] = { "Idle             ",
                            "WIFI Connecting  ",
                            "Requesting Server",
                            "Transferring     ",
                            "Completing       ",
                            "Completed        "};


void StartTransfer() {
  int8_t status;
  int8_t error;
  uint16_t block;
  uint16_t time;
  
  clrscr();
  SendCommand(CMD_RESETTIMER_S);
  
  printf("Starting...");
  StartTFTP();
  printf("ok\n");
  
  gotoxy(0,2);
  printf("Status:\n");
  printf("Block:");
  
  do {
    SendCommand(CMD_TFTPSTATUS);
  #ifndef TESTBUILD
    status = GetParam8();
    error = GetParam8();
    block = (uint16_t)GetParam16();
  #else
    status = 5;
    error = -7;
    block = 10;
  #endif  
    
    gotoxy(7,2);
    printf("%s",STATUSMSG[status]);
    gotoxy(6,3);
    if (status>=3) printf("%u",block);
  } while (status!=5);

  gotoxy(0,5);  
  if (error!=-1) printf("Error:%d\n",error);
  else printf("No error\n");
  
  SendCommand(CMD_GETTIMER_S);
  time = GetParam16();
  printf("Elapsed Time = %us",time);
}
