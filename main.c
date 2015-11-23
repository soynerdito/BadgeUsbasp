/*
  USBasp - USB in-circuit programmer for Atmel AVR controllers

  Thomas Fischl <tfischl@gmx.de>

  License........: GNU GPL v2 (see Readme.txt)
  Target.........: ATMega*8 at 12 or 16 MHz
  Creation Date..: 2005-02-20
  Last change....: 2008-04-02 for metaboard hardware by Christian Starkjohann

  PC2 SCK speed option. GND  -> slow (8khz SCK),
                        open -> fast (<= 250kHz SCK)
*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>

#include "usbdrv.h"
#include "isp.h"
#include "clock.h"

#define USBASP_FUNC_CONNECT     1
#define USBASP_FUNC_DISCONNECT  2
#define USBASP_FUNC_TRANSMIT    3
#define USBASP_FUNC_READFLASH   4
#define USBASP_FUNC_ENABLEPROG  5
#define USBASP_FUNC_WRITEFLASH  6
#define USBASP_FUNC_READEEPROM  7
#define USBASP_FUNC_WRITEEEPROM 8
#define USBASP_FUNC_SETLONGADDRESS 9

#define PROG_STATE_IDLE         0
#define PROG_STATE_WRITEFLASH   1
#define PROG_STATE_READFLASH    2
#define PROG_STATE_READEEPROM   3
#define PROG_STATE_WRITEEEPROM  4

#define PROG_BLOCKFLAG_FIRST    1
#define PROG_BLOCKFLAG_LAST     2

#if METABOARD_HARDWARE
#  define SPEED_SELECTION_INPORT  PINC
#  define SPEED_SELECTION_OUTPORT PORTC
#  define SPEED_SELECTION_BIT     PC2
#else
#  define SPEED_SELECTION_INPORT  PIND
#  define SPEED_SELECTION_OUTPORT PORTD
#  define SPEED_SELECTION_BIT     PD7   /* re-use "upload" jumper */
#endif

static inline void ledRedOn(void)
{
  PORTC &= ~(1 << PC1); /* turn on LED (active low) */
#if METABOARD_HARDWARE
  PORTC |= (3 << PC3);  /* turn on optional power supply for programming socket */
  DDRB |= (1 << PB1);   /* turn on optional clock for programming socket */
  uchar i;
  for(i=0;i<15;i++)     /* wait 150 ms for target's startup time */
    _delay_ms(10);      /* this call has a limited argument range */
#endif
}

static inline void ledRedOff(void)
{
#if METABOARD_HARDWARE
  DDRB &= ~(1 << PB1);  /* turn off optional clock for programming socket */
  PORTC &= ~(3 << PC3); /* turn off optional power supply for programming socket */
#endif
  PORTC |= (1 << PC1);  /* turn off LED */
}

#define ledGreenOn()  PORTC &= ~(1 << PC0)
#define ledGreenOff() PORTC |= (1 << PC0)

static uchar replyBuffer[8];

static uchar prog_state = PROG_STATE_IDLE;

static uchar prog_address_newmode = 0;
static unsigned long prog_address;
static unsigned int prog_nbytes = 0;
static unsigned int prog_pagesize;
static uchar prog_blockflags;
static uchar prog_pagecounter;


uchar usbFunctionSetup(uchar data[8]) {

  uchar len = 0;

  if(data[1] == USBASP_FUNC_CONNECT){

    /* set SCK speed */
    if ((SPEED_SELECTION_INPORT & (1 << SPEED_SELECTION_BIT)) == 0) {
      ispSetSCKOption(ISP_SCK_SLOW);
    } else {
      ispSetSCKOption(ISP_SCK_FAST);
    }

    /* set compatibility mode of address delivering */
    prog_address_newmode = 0;

    ledRedOn();
    ispConnect();

  } else if (data[1] == USBASP_FUNC_DISCONNECT) {
    ispDisconnect();
    ledRedOff();

  } else if (data[1] == USBASP_FUNC_TRANSMIT) {
    replyBuffer[0] = ispTransmit(data[2]);
    replyBuffer[1] = ispTransmit(data[3]);
    replyBuffer[2] = ispTransmit(data[4]);
    replyBuffer[3] = ispTransmit(data[5]);
    len = 4;

  } else if (data[1] == USBASP_FUNC_READFLASH) {
    
    if (!prog_address_newmode)
      prog_address = (data[3] << 8) | data[2];
    
    prog_nbytes = (data[7] << 8) | data[6];
    prog_state = PROG_STATE_READFLASH;
    len = 0xff; /* multiple in */

  } else if (data[1] == USBASP_FUNC_READEEPROM) {
    
    if (!prog_address_newmode)
       prog_address = (data[3] << 8) | data[2];

    prog_nbytes = (data[7] << 8) | data[6];
    prog_state = PROG_STATE_READEEPROM;
    len = 0xff; /* multiple in */

  } else if (data[1] == USBASP_FUNC_ENABLEPROG) {
    replyBuffer[0] = ispEnterProgrammingMode();;
    len = 1;

  } else if (data[1] == USBASP_FUNC_WRITEFLASH) {

    if (!prog_address_newmode)
      prog_address = (data[3] << 8) | data[2];

    prog_pagesize = data[4];
    prog_blockflags = data[5] & 0x0F;
    prog_pagesize += (((unsigned int)data[5] & 0xF0)<<4);
    if (prog_blockflags & PROG_BLOCKFLAG_FIRST) {
      prog_pagecounter = prog_pagesize;
    }
    prog_nbytes = (data[7] << 8) | data[6];
    prog_state = PROG_STATE_WRITEFLASH;
    len = 0xff; /* multiple out */

  } else if (data[1] == USBASP_FUNC_WRITEEEPROM) {

    if (!prog_address_newmode)
      prog_address = (data[3] << 8) | data[2];

    prog_pagesize = 0;
    prog_blockflags = 0;
    prog_nbytes = (data[7] << 8) | data[6];
    prog_state = PROG_STATE_WRITEEEPROM;
    len = 0xff; /* multiple out */

  } else if(data[1] == USBASP_FUNC_SETLONGADDRESS) {

    /* set new mode of address delivering (ignore address delivered in commands) */
    prog_address_newmode = 1;
    /* set new address */
    prog_address = *((unsigned long*)&data[2]);
  }

  usbMsgPtr = replyBuffer;

  return len;
}


uchar usbFunctionRead(uchar *data, uchar len) {

  uchar i;

  /* check if programmer is in correct read state */
  if ((prog_state != PROG_STATE_READFLASH) &&
      (prog_state != PROG_STATE_READEEPROM)) {
    return 0xff;
  }

  /* fill packet */
  for (i = 0; i < len; i++) {
    if (prog_state == PROG_STATE_READFLASH) {
      data[i] = ispReadFlash(prog_address);
    } else {
      data[i] = ispReadEEPROM(prog_address);
    }
    prog_address++;
  }

  /* last packet? */
  if (len < 8) {
    prog_state = PROG_STATE_IDLE;
  }

  return len;
}


uchar usbFunctionWrite(uchar *data, uchar len) {

  uchar retVal = 0;
  uchar i;

  /* check if programmer is in correct write state */
  if ((prog_state != PROG_STATE_WRITEFLASH) &&
      (prog_state != PROG_STATE_WRITEEEPROM)) {
    return 0xff;
  }


  for (i = 0; i < len; i++) {

    if (prog_state == PROG_STATE_WRITEFLASH) {
      /* Flash */

      if (prog_pagesize == 0) {
	/* not paged */
	ispWriteFlash(prog_address, data[i], 1);
      } else {
	/* paged */
	ispWriteFlash(prog_address, data[i], 0);
	prog_pagecounter --;
	if (prog_pagecounter == 0) {
	  ispFlushPage(prog_address, data[i]);
	  prog_pagecounter = prog_pagesize;
	}
      }

    } else {
      /* EEPROM */
      ispWriteEEPROM(prog_address, data[i]);
    }

    prog_nbytes --;

    if (prog_nbytes == 0) {
      prog_state = PROG_STATE_IDLE;
      if ((prog_blockflags & PROG_BLOCKFLAG_LAST) &&
	  (prog_pagecounter != prog_pagesize)) {

	/* last block and page flush pending, so flush it now */
	ispFlushPage(prog_address, data[i]);
      }
	  
	  retVal = 1; // Need to return 1 when no more data is to be received
    }

    prog_address ++;
  }

  return retVal;
}

#define UTIL_BIN4(x)        (uchar)((0##x & 01000)/64 + (0##x & 0100)/16 + (0##x & 010)/4 + (0##x & 1))
#define UTIL_BIN8(hi, lo)   (uchar)(UTIL_BIN4(hi) * 16 + UTIL_BIN4(lo))

static void timer1Init(void)
{
#if METABOARD_HARDWARE
  TCCR1A = UTIL_BIN8(1000, 0010); /* OC1A = PWM out, OC1B disconnected */
  TCCR1B = UTIL_BIN8(0001, 1001); /* wgm 14: TOP = ICR1, prescaler = 1 */
  ICR1 = F_CPU / 1000000 - 1;     /* TOP value for 1 MHz */
  OCR1A = F_CPU / 2000000 - 1;    /* 50% duty cycle */
#endif
}

int main(void)
{
  uchar   i;

  usbInit();
  /* enforce USB re-enumerate: */
  usbDeviceDisconnect();  /* do this while interrupts are disabled */
  for(i=0;--i;){          /* fake USB disconnect for > 250 ms */
    _delay_ms(1);
  }
  usbDeviceConnect();
  PORTC |= (1 << PC0) | (1 << PC1); /* default status for LEDs = off */
  DDRC |= (1 << PC0) | (1 << PC1);  /* LEDs are output */
#if METABOARD_HARDWARE
  DDRC |= (1 << PD3) | (1 << PD4);  /* prog socket power supply is output */
#endif
  SPEED_SELECTION_OUTPORT |= (1 << SPEED_SELECTION_BIT);    /* enable pull-up */
  timer1Init();
  clockInit();          /* init timer */

  ispSetSCKOption(ISP_SCK_FAST);
  sei();
  for(;;){	        /* main event loop */
    usbPoll();
  }
  return 0;
}


