#ifndef _UI_WND_H
#define _UI_WND_H

void wnd_ResetScrollWindow();
void wnd_DrawBox(uint8_t x,uint8_t y,uint8_t width,uint8_t height);
void wnd_DrawWindow(uint8_t x,uint8_t y,uint8_t width,uint8_t height,const char* title, bool isActive,bool clearContentArea);

#endif
