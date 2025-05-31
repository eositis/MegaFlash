#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "hardware/timer.h"
#include "usbserial.h"

//
// Routines to access USB serial port for file transfer and
// basic user interface
//

/////////////////////////////////////////////////////////////////
//Send single byte to host
//
//input: ch - byte to be sent
//
void usb_putchar(const char ch) {
  stdio_usb.out_chars(&ch,1);
}

/////////////////////////////////////////////////////////////////
//Send binary data to host
//
//input: buf - pointer to data buffer
//       len - number of bytes
//
void usb_putraw(const char* buf,const int len) {
  stdio_usb.out_chars(buf,len);
}

/////////////////////////////////////////////////////////////////
//Get a single byte from host if there is one available within a timeout
//
//input: timeout_us - timeout period in us
//
//output: byte received or
//        -1 if timeout
//        
int usb_getchar_timeout_us(const uint32_t timeout_us){
  char ch;
  uint32_t start,elapsed;
  
  start = time_us_32();
  do {
    int count=stdio_usb.in_chars(&ch,1);
    if (count>0) return ch;
    
    elapsed = time_us_32() - start;
  }while (elapsed<timeout_us);
  
  return -1;
}



/////////////////////////////////////////////////////////////////
//Get a single key from host if there is one available within a timeout
//VT-100 and Control Characters are ignored.
//
//input: timeout_us - timeout period in us
//
//output: key received or
//        -1 if timeout
//      
int usb_getkey_timeout_us(const uint32_t timeout_us) {
  const int escape = 27;  
  
  int ch;
again:  
  ch = usb_getchar_timeout_us(timeout_us);
  if (ch==-1) return -1; //timeout
  
  //Escape character, discard all subsequent input for 20ms
  //to ignore all VT-100 escape codes
  if (ch==escape) {
    usb_discard_duration_us(20*1000);
    goto again;
  } 
  
  return ch;
}

int usb_getkey() {
  int key;
  
  do {
    key = usb_getkey_timeout_us(1000);
  }while(key==-1);
  
  return key;
}


/////////////////////////////////////////////////////////////////
//Wait for mult-bytes input from host for timeout_us microseconds
//The function returns when either the buffer is full or timeout 
//
//input: buf - pointer to buffer
//       len - size of the buffer 
//       timeout_us - timeout period in us
//
//output: number of byte received
//     
uint32_t usb_getraw_timeout(char *buf,const uint32_t len,const uint32_t timeout_us) {
  uint32_t receivedCount = 0;
  uint32_t start,elapsed; 
  
  start = time_us_32();
  
  do {
    int count = stdio_usb.in_chars(buf+receivedCount, len-receivedCount);
    if (count>0) receivedCount += count;
    
    elapsed = time_us_32() - start;
  }while (receivedCount<len && elapsed<timeout_us);
  
  return receivedCount;
}

/////////////////////////////////////////////////////////////////
//Get String iput from user with basic line editing
//The function returns when the user hits Enter or Ctrl-C
//
//input: buf - pointer to buffer
//       len - size of the buffer 
//       cancelled - pointer to bool, to tell if user hit Ctrl-C.
//                   It can be null if not used.
//
//output: number of character received.
//        the string is null-terminated
// 
uint32_t usb_getstring(char* buf,const uint32_t len,bool *cancelled) {
  uint32_t count = 0; 
  int ch; 
  const uint32_t limit = len - 1; //leave one space for null character
  const int ctrlc = 3;
  const int escape = 27;

  while(1) {
    ch = usb_getchar_timeout_us(0);
    if (ch>=0) {
      //stop if carriage return (CR) or ctrl-c
      if (ch=='\r' || ch==ctrlc) break;
      
      //Backspace
      if (ch=='\b' && count>0) {
        --count;
        usb_putraw("\b \b",3);  //remove last character from terminal
        continue;
      }
      
      //Escape character, discard all subsequent input for 20ms
      //to ignore all VT-100 escape codes
      if (ch==escape) {
        usb_discard_duration_us(20*1000);
      }
      
      //ignore all other control characters,
      if (ch<32) continue;      
      
      //Put the character to buffer if it is not full
      if (count<limit) {
        buf[count++]=ch;
        usb_putchar(ch);
      }
    }
  } 
  
  //Ctrl-C hit, empty the buffer
  if (ch==ctrlc) {
    count = 0;  //cancel all input
  }

  //Set up cancelled flag
  if (cancelled) *cancelled = (ch==ctrlc);
  
  buf[count]='\0';  //null terminate the string
  return count;
}


/////////////////////////////////////////////////////////////////
//Discard everything from input buffer
//
void usb_discard() {
  char buf[256];
  int count;
  do {
    count = stdio_usb.in_chars(buf,256);
  }while (count>0);
}

/////////////////////////////////////////////////////////////////
//Discard all subsequent input for duration_us microseconds.
//
//input: duration_us - in microseconds
//
void usb_discard_duration_us(const uint32_t duration_us){
  uint32_t start,elapsed;
  char buf[256];
  
  start = time_us_32();
  do {
    stdio_usb.in_chars(buf,256);
    elapsed = time_us_32() - start;
  }while (elapsed<duration_us);
}



