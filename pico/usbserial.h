#ifndef _USBSERIAL_H
#define _USBSERIAL_H

#ifdef __cplusplus
extern "C" {
#endif

void usb_putchar(const char ch);
void usb_putraw(const char* buf,const int len);

int usb_getchar_timeout_us(const uint32_t timeout_us);
uint32_t usb_getraw_timeout(char *buf,const uint32_t len,const uint32_t timeout_us);
int usb_getkey();
int usb_getkey_timeout_us(const uint32_t timeout_us);
uint32_t usb_getstring(char* buf,const uint32_t len,bool *cancelled);

void usb_discard_duration_us(const uint32_t timeout_us);
void usb_discard();

#ifdef __cplusplus
}
#endif

#endif

