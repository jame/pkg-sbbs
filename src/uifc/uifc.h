/* uifc.h */

/* Rob Swindell's Text-mode User Interface Library */

/* $Id: uifc.h,v 1.75 2006/09/06 22:24:59 deuce Exp $ */

/****************************************************************************
 * @format.tab-size 4		(Plain Text/Source Code File Header)			*
 * @format.use-tabs true	(see http://www.synchro.net/ptsc_hdr.html)		*
 *																			*
 * Copyright 2005 Rob Swindell - http://www.synchro.net/copyright.html		*
 *																			*
 * This library is free software; you can redistribute it and/or			*
 * modify it under the terms of the GNU Lesser General Public License		*
 * as published by the Free Software Foundation; either version 2			*
 * of the License, or (at your option) any later version.					*
 * See the GNU Lesser General Public License for more details: lgpl.txt or	*
 * http://www.fsf.org/copyleft/lesser.html									*
 *																			*
 * Anonymous FTP access to the most recent released source is available at	*
 * ftp://vert.synchro.net, ftp://cvs.synchro.net and ftp://ftp.synchro.net	*
 *																			*
 * Anonymous CVS access to the development source and modification history	*
 * is available at cvs.synchro.net:/cvsroot/sbbs, example:					*
 * cvs -d :pserver:anonymous@cvs.synchro.net:/cvsroot/sbbs login			*
 *     (just hit return, no password is necessary)							*
 * cvs -d :pserver:anonymous@cvs.synchro.net:/cvsroot/sbbs checkout src		*
 *																			*
 * For Synchronet coding style and modification guidelines, see				*
 * http://www.synchro.net/source.html										*
 *																			*
 * You are encouraged to submit any modifications (preferably in Unix diff	*
 * format) via e-mail to mods@synchro.net									*
 *																			*
 * Note: If this box doesn't appear square, then you need to fix your tabs.	*
 ****************************************************************************/

#ifndef _UIFC_H_
#define _UIFC_H_

#include <time.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
/* OS Specific */
#if defined(_WIN32)
    #include <windows.h>
#endif
#if defined(__unix__)
	#include <sys/param.h>	/* PATH_MAX */
#endif

#if defined(__unix__) && !defined(stricmp)
    #define stricmp strcasecmp
	#define strnicmp strncasecmp
#endif

#if !defined(FREE_AND_NULL)
	#define FREE_AND_NULL(x)			if(x!=NULL) { free(x); x=NULL; }
#endif

#if !defined(MAX_PATH)	/* maximum path length */
	#if defined MAXPATHLEN
		#define MAX_PATH MAXPATHLEN	/* clib.h */
	#elif defined PATH_MAX
		#define MAX_PATH PATH_MAX
	#elif defined _MAX_PATH
		#define MAX_PATH _MAX_PATH
	#else
		#define MAX_PATH 260		
	#endif
#endif

#define MAX_OPTS	10000
#define MSK_ON		0xf0000000
#define MSK_OFF 	0x0fffffff
#define MSK_INS 	0x10000000
#define MSK_DEL 	0x20000000
#define MSK_GET 	0x30000000
#define MSK_PUT 	0x40000000
#define MSK_EDIT 	0x50000000
/* Dont forget, negative return values are used for extended keys (if WIN_EXTKEYS used)! */
#define MAX_OPLN	75		/* Maximum length of each option per menu call */
#define MAX_BUFS	7		/* Maximum number of screen buffers to save */
#define MIN_LINES   14      /* Minimum number of screen lines supported */
#define MAX_LINES   60      /* Maximum number of screen rows supported */ 
#define MAX_BFLN	80*MAX_LINES*2	/* Maximum size of screen buffers */

#ifndef uint
#define uint unsigned int
#endif

							/**************************/
                            /* Bits in uifcapi_t.mode */
							/**************************/
#define UIFC_INMSG	(1<<0)	/* Currently in Message Routine non-recursive */
#define UIFC_MOUSE	(1<<1)	/* Mouse installed and available */
#define UIFC_MONO	(1<<2)	/* Force monochrome mode */
#define UIFC_COLOR	(1<<3)	/* Force color mode */
#define UIFC_IBM	(1<<4)	/* Force use of IBM charset	*/
#define UIFC_NOCTRL	(1<<5)	/* Don't allow useage of CTRL keys for movement 
							 * etc in menus (Still available in text boxes) */

							/*******************************/
                            /* Bits in uifcapi_t.list mode */
							/*******************************/
#define WIN_ORG 	(1<<0)	/* Original menu - destroy valid screen area */
#define WIN_SAV 	(1<<1)	/* Save existing text and replace when finished */
#define WIN_ACT 	(1<<2)	/* Menu remains active after a selection */
#define WIN_L2R 	(1<<3)	/* Center the window based on 'width'   */
#define WIN_T2B 	(1<<4)	/* Center the window based on 'height'  */
#define WIN_INS 	(1<<5)	/* Allows user to user insert key */
#define WIN_INSACT	(1<<6)	/* Remains active after insert key */
#define WIN_DEL 	(1<<7)	/* Allows user to use delete key */
#define WIN_DELACT	(1<<8)	/* Remains active after delete key */
#define WIN_ESC 	(1<<9)	/* Screen is active when escape is hit			 */
#define WIN_RHT 	(1<<10) /* Place window against right side of screen */
#define WIN_BOT 	(1<<11) /* Place window against botton of screen */
#define WIN_GET 	(1<<12) /* Allows F5 to Get a menu item */
#define WIN_PUT 	(1<<13) /* Allows F6 to Put a menu item */
#define WIN_CHE 	(1<<14) /* Stay active after escape if changes */
#define WIN_XTR 	(1<<15) /* Add extra line at end for inserting at end */
#define WIN_DYN 	(1<<16) /* Dynamic window - return at least every second */
#define WIN_HLP 	(1<<17) /* Parse 'Help codes' */
#define WIN_PACK 	(1<<18) /* Pack text in window (No padding) */
#define WIN_IMM 	(1<<19) /* Draw window and return immediately */
#define WIN_FAT		(1<<20)	/* Do not pad outside borders */
#define WIN_REDRAW	(1<<21) /* Force redraw on dynamic window */
#define WIN_NODRAW	(1<<22) /* Force not to redraw on dynamic window */
#define WIN_EXTKEYS	(1<<23) /* Return on any keypress... if it's not handled internally
							 * Return value is -2 - keyvalue */
#define WIN_NOBRDR	(1<<24)	/* Do not draw a border around the window */
#define WIN_FIXEDHEIGHT	(1<<25)	/* Use list_height from uifc struct */
#define WIN_UNGETMOUSE  (1<<26) /* If the mouse is clicked outside the window, */
								/* Put the mouse event back into the event queue */
#define WIN_EDIT	(1<<27)	/* Allow F2 to edit a menu item */
#define WIN_EDITACT	(1<<28)	/* Remain active after edit key */
#define WIN_INACT	(1<<29)	/* Draw window inactive... intended for use with WIN_IMM */

#define WIN_MID WIN_L2R|WIN_T2B  /* Place window in middle of screen */

#define SCRN_TOP	3
#define SCRN_LEFT	5
#define SCRN_RIGHT 	((int)api->scrn_width-4)

								/* Bits in 'mode' for getkey and getstr     */
#define K_UPPER 	(1L<<0) 	/* Converts all letters to upper case		*/
#define K_UPRLWR	(1L<<1) 	/* Upper/Lower case automatically			*/
#define K_NUMBER	(1L<<2) 	/* Allow numbers only						*/
#define K_WRAP		(1L<<3) 	/* Allows word wrap 						*/
#define K_MSG		(1L<<4) 	/* Allows ANSI, ^N ^A ^G					*/
#define K_SPIN		(1L<<5) 	/* Spinning cursor (same as SPIN)			*/
#define K_LINE		(1L<<6) 	/* Input line (inverse color)				*/
#define K_EDIT		(1L<<7) 	/* Edit string passed						*/
#define K_CHAT		(1L<<8) 	/* In chat multi-chat						*/
#define K_NOCRLF	(1L<<9) 	/* Don't print CRLF after string input      */
#define K_ALPHA 	(1L<<10)	/* Only allow alphabetic characters 		*/
#define K_SCANNING	(1L<<11)	/* UPC Scanner is active... return on '%'	*/
#define K_TABEXIT	(1L<<12)	/* Return on TAB or BACKTAB			    	*/
#define K_DECIMAL	(1L<<13)	/* Allow floating point numbers only		*/
#define K_DEUCEEXIT	(1L<<14)	/* Return whenever Deuce wants to exit		*/
								/* Returns on up/down/F2					*/
#define K_MOUSEEXIT	(1L<<15)	/* Returns when mouse is clicked outside of */
								/* Input area (NOT outside of window!)		*/
								/* And ungets the mouse event.				*/
#define K_PASSWORD	(1L<<16)	/* Does not display text while editing		*/

						/* Bottom line elements */
#define BL_INS      (1<<0)  /* INS key */
#define BL_DEL      (1<<1)  /* DEL key */
#define BL_GET      (1<<2)  /* Get key */
#define BL_PUT      (1<<3)  /* Put key */
#define BL_EDIT     (1<<4)  /* Edit key */
#define BL_HELP     (1<<5)  /* Help key */

#define HELPBUF_SIZE 4000

#ifndef _GEN_DEFS_H
									/* Control characters */
#define STX 	0x02				/* Start of text			^B	*/
#define ETX 	0x03				/* End of text				^C	*/
#define BS		'\b'				/* Back space				^H	*/
#define TAB 	'\t'				/* Horizontal tabulation	^I	*/
#define LF		'\n'				/* Line feed				^J	*/
#define FF		0x0c				/* Form feed				^L	*/
#define CR		'\r'				/* Carriage return			^M	*/
#define ESC 	0x1b				/* Escape					^[	*/
#define DEL     0x7f                /* Delete                   ^BS */

enum {
	 CTRL_A=1
	,CTRL_B
	,CTRL_C
	,CTRL_D	
	,CTRL_E
	,CTRL_F
	,CTRL_G
	,CTRL_H
	,CTRL_I
	,CTRL_J
	,CTRL_K
	,CTRL_L
	,CTRL_M
	,CTRL_N
	,CTRL_O
	,CTRL_P
	,CTRL_Q
	,CTRL_R
	,CTRL_S
	,CTRL_T
	,CTRL_U
	,CTRL_V
	,CTRL_W
	,CTRL_X
	,CTRL_Y
	,CTRL_Z
};

#endif

#ifndef uchar				/* Short-hand for unsigned data types */
#define uchar unsigned char
#endif
#ifndef uint
#define uint unsigned int
#endif
#ifndef ulong
#define ulong unsigned long
#endif

#ifndef BOOL
#define BOOL    int
#ifndef TRUE
#define TRUE    1
#endif
#ifndef FALSE
#define FALSE   0
#endif
#endif

typedef struct {
	int		left,top,right,bot;
	int		*cur,*bar;
    uchar*	buf;
} win_t;

typedef struct {
/****************************************************************************/
/* Size of the structure (for version compatibility verification).			*/
/****************************************************************************/
    size_t  size;
/****************************************************************************/
/* Controls general UIFC library behavior.									*/
/****************************************************************************/
    long    mode;
/****************************************************************************/
/* Set to TRUE when changes to data have been made by input function.		*/ 
/****************************************************************************/
    BOOL    changes;
/****************************************************************************/
/* The overlapped-window save buffer number.								*/
/****************************************************************************/
    uint    savnum;
/****************************************************************************/
/* The current overlapped-window save buffer depth.							*/
/****************************************************************************/
    uint    savdepth;
/****************************************************************************/
/* Screen length															*/
/****************************************************************************/
    uint    scrn_len;
/****************************************************************************/
/* Screen Width 															*/
/****************************************************************************/
    uint    scrn_width;
/****************************************************************************/
/* ESC key delay for curses													*/
/****************************************************************************/
    uint    esc_delay;
/****************************************************************************/
/* Alternative method of setting current help text.							*/
/****************************************************************************/
    char*   helpbuf;
/****************************************************************************/
/* Location of the help data and index files.								*/
/****************************************************************************/
    char    helpdatfile[MAX_PATH+1];
    char    helpixbfile[MAX_PATH+1];
/****************************************************************************/
/* Help and exit button locations for current/last window					*/
/****************************************************************************/
	int		buttony;
	int		exitstart;
	int		exitend;
	int		helpstart;
	int		helpend;
/****************************************************************************/
/* List height for WIN_FIXEDHEIGHT lists.									*/
/****************************************************************************/
	int		list_height;

/****************************************************************************/
/* Colours for the various bits												*/
/****************************************************************************/
	uchar	hclr,lclr,bclr,cclr,lbclr;

/****************************************************************************/
/* Have we initialized successfully?										*/
/****************************************************************************/
	BOOL	initialized;

/****************************************************************************/
/* Exit/uninitialize function.												*/
/****************************************************************************/
    void    (*bail) (void);
/****************************************************************************/
/* Fill the screen with the appropriate background attribute.				*/
/* str is the title for the application banner.								*/
/* Returns 0 on success, non-zero on failure.								*/
/****************************************************************************/
    int     (*scrn) (char* str);
/****************************************************************************/
/* Popup a message, maybe wait for the user to hit a key or click button.	*/
/****************************************************************************/
    void    (*msg)  (char* str);
/****************************************************************************/
/* Popup/down a status message.												*/
/* str is the message to display on popup.									*/
/* if str==NULL, then the the status is to be cleared (popdown).			*/
/****************************************************************************/
    void    (*pop)  (char* str);
/****************************************************************************/
/* General menu function.													*/
/* mode contains WIN_* flags to control display and functionality.			*/
/* left, top and width specify desired screen locations and window size.	*/
/* cur is a pointer to the current (default) option.						*/
/* bar is a pointer to the current location of the lightbar (which used).	*/
/* title is the caption for the menu.										*/
/* Menus can centered left to right and top to bottom automatically.		*/
/* mode bits are set with macros WIN_*.										*/
/* option is an array of char arrays, first element of last char array		*/
/* must be NULL.															*/
/* Returns the 0-based selected option number, -1 for ESC, or the selected	*/
/* option number OR'd with MSK_INS, MSK_DEL, MSK_GET, MSK_PUT, or MSK_EDIT.	*/
/****************************************************************************/
    int     (*list) (int mode, int left, int top, int width, int* dflt
                        ,int* bar, char *title, char** option);
/****************************************************************************/
/* Windowed string input routine.											*/
/* mode contains WIN_* flags to control display and functionality.			*/
/* left and top specify desired screen location.							*/
/* prompt is displayed before the input is requested.						*/
/* str is the string to input or edit.										*/
/* len is the maximum length of the string.									*/
/* kmode contains flags that control the string input (K_* macros).			*/
/* This function sets uifcapi_t.changes to TRUE if the string is modified.	*/
/* Returns the length of the string or -1 on escape/abort.					*/
/****************************************************************************/
    int     (*input)(int mode, int left, int top, char* prompt, char* str
            	        ,int len, int kmode);
/****************************************************************************/
/* Sets the current help index by source code file and line number.			*/
/****************************************************************************/
    void    (*sethelp)(int line, char* file);

/****************************************************************************/
/* Shows the current help text												*/
/****************************************************************************/
    void    (*showhelp)(void);
	
/****************************************************************************/
/* Shows a scrollable text buffer - optionally parsing "help markup codes"	*/
/****************************************************************************/
	void	(*showbuf)(int mode, int left, int top, int width, int height
							,char *title, char *hbuf, int *curp, int *barp);

/****************************************************************************/
/* Updates time in upper left corner of screen with current time in ASCII/  */
/* Unix format																*/
/****************************************************************************/
	void	(*timedisplay)(BOOL force);

/****************************************************************************/
/* Displays the bottom line using the BL_* macros							*/
/****************************************************************************/
    void	(*bottomline)(int line);

/****************************************************************************/
/* String input/exit box at a specified position							*/
/****************************************************************************/
	int		(*getstrxy)(int left, int top, int width, char *outstr, int max
							,long mode, int *lastkey);

/****************************************************************************/
/* Formatted print with attribute											*/
/****************************************************************************/
	int		(*printf)(int x, int y, unsigned attr, char *fmat, ...);

} uifcapi_t;

/****************************************************************************/
/* Initialization routines for each UIFC implementation.					*/
/* Returns 0 on success, non-zero on failure.								*/
/****************************************************************************/
int uifcini(uifcapi_t*);	/* Original implementation based on conio		*/
int uifcinix(uifcapi_t*);	/* Standard I/O implementation					*/
int uifcini32(uifcapi_t*);	/* conio/curses implementation					*/
/****************************************************************************/

#endif /* Don't add anything after this line! */