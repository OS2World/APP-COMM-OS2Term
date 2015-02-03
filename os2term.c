#define INCL_DOS
#define INCL_DOSDEVIOCTL
#define INCL_VIO
#define INCL_KBD
#include <os2.h>


/* baud rate at which we wish to use the com port */
#define BAUD_RATE 38400

/* size of incoming character buffer we wish to use */
#define COMM_BUFFER_SIZE 2048


/* GLOBAL VARIABLES */
int head,			/* index to the last char in buffer */
  tail; 			/* index to first char in buffer */
char buffer[COMM_BUFFER_SIZE];	/* incoming character buffer */
HFILE PortHandle;		/* OS/2 file handle for COM port */
TID RecvThreadID;		/* Thread ID of receive-character thread */


/*
 * our receive-character thread; all it does is wait for a
 * character to come in on the com port.  when one does, it
 * suspends the current process with DosEnterCritSec() and
 * places the character in the buffer.
 *
 * Purists will note that using DosEnterCritSec() instead of
 * semaphores is not "clean" or "true" multi-threading, but I chose
 * this method because it gave the largest performance boost.
 */
VOID async_isr (ULONG ulThreadArg)
{
  ULONG BytesRead;		/* num. bytes read from last DosRead() call */
  char ch;			/* char read in from last DosRead() call */

  /* endless loop */
  while (1)
    {
      /* read character; this will block until a char is available */
      DosRead (PortHandle, (PVOID) & ch, 1L, &BytesRead);

      /* if a character was actually read in... */
      if (BytesRead)
	{
	  /* suspend all other processing */
	  DosEnterCritSec ();

	  /* put char in buffer and adjust indices */
	  buffer[head] = ch;
	  head++;
	  if (head == COMM_BUFFER_SIZE)
	    head = 0;

	  /* release suspended processes */
	  DosExitCritSec ();
	}
    }
}


/* This function outputs one character to the com port */
void outcomch (char ch)
{
  ULONG BytesWritten;		/* unless but required parameter */

  DosWrite (PortHandle, &ch, sizeof (ch), &BytesWritten);
}


/* return next char in receive buffer, or 0 for none available */
char peek1c (void)
{
  return ((head != tail) ? buffer[tail] : 0);
}


/* This function returns one character from the com port, or a zero if
 * no character is waiting
 */
char get1c (void)
{
  /* temp var to hold char for returning if one is available */
  char c1;

  if (head != tail)
    {
      c1 = buffer[tail++];
      if (tail == COMM_BUFFER_SIZE)
	tail = 0;
      return (c1);
    }
  else
    return (0);
}


/* This returns a value telling if there is a character waiting in the com
 * buffer.
 */
int comhit (void)
{
  return (head != tail);
}


/* This function clears the com buffer */
void dump (void)
{
  head = tail = 0;
}


/* This function sets the com speed to that passed */
void set_baud (unsigned int rate)
{
  USHORT Baud = (USHORT) rate;

  /*
   * OS/2 2.x standard COM drivers only support up to 38400 bps
   */
  if ((rate <= 38400) && (rate >= 50))
    DosDevIOCtl (PortHandle, IOCTL_ASYNC, ASYNC_SETBAUDRATE,
		 &Baud, sizeof (USHORT), NULL, NULL, 0L, NULL);
}


/* This function sets the DTR pin to the status given */
void dtr (int i)
{
  MODEMSTATUS ms;
  UINT data;

  ms.fbModemOn = i ? DTR_ON : 0;
  ms.fbModemOff = i ? 255 : DTR_OFF;
  DosDevIOCtl (PortHandle, IOCTL_ASYNC, ASYNC_SETMODEMCTRL, &ms,
	       sizeof (ms), NULL, &data, sizeof (data), NULL);
}


/* This function sets the RTS pin to the status given */
void rts (int i)
{
  MODEMSTATUS ms;
  UINT data;

  ms.fbModemOn = i ? RTS_ON : 0;
  ms.fbModemOff = i ? 255 : RTS_OFF;
  DosDevIOCtl (PortHandle, IOCTL_ASYNC, ASYNC_SETMODEMCTRL, &ms,
	       sizeof (ms), NULL, &data, sizeof (data), NULL);
}


/* This function initializes the com buffer, setting up the interrupt,
 * and com parameters
 */
void initport (int port_num)
{
  char s[10] = "COMx";
  APIRET rc;
  ULONG action;
  LINECONTROL lctl;
  DCBINFO dcb;

  /* open com port */
  s[3] = port_num + '0';
  if ((rc = DosOpen (s, &PortHandle, &action, 0L, 0, FILE_OPEN,
		     OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYREADWRITE |
		     OPEN_FLAGS_FAIL_ON_ERROR, 0L)))
    {
      VioWrtTTY ("Cannot open com port\r\n", 22, 0);
      DosExit (EXIT_PROCESS, 1);
    }

  /* set line to no parity, 8 databits, 1 stop bit */
  lctl.bParity = 0;
  lctl.bDataBits = 8;
  lctl.bStopBits = 0;
  lctl.fTransBreak = 0;
  if ((rc = DosDevIOCtl (PortHandle, IOCTL_ASYNC, ASYNC_SETLINECTRL,
			 &lctl, sizeof (LINECONTROL), NULL, NULL, 0L, NULL)))
    {
      VioWrtTTY ("Cannot set line settings\r\n", 26, 0);
      DosExit (EXIT_PROCESS, 1);
    }

  /* set device control block info */
  dcb.usWriteTimeout = 0;
  dcb.usReadTimeout = 0;
  dcb.fbCtlHndShake = MODE_DTR_CONTROL | MODE_CTS_HANDSHAKE;
  dcb.fbFlowReplace = MODE_RTS_CONTROL;
  dcb.fbTimeout = MODE_NO_WRITE_TIMEOUT | MODE_WAIT_READ_TIMEOUT;
  dcb.bErrorReplacementChar = 0x00;
  dcb.bBreakReplacementChar = 0x00;
  dcb.bXONChar = 0x11;
  dcb.bXOFFChar = 0x13;
  if ((rc = DosDevIOCtl (PortHandle, IOCTL_ASYNC, ASYNC_SETDCBINFO, &dcb,
			 sizeof (DCBINFO), 0L, NULL, 0L, NULL)))
    {
      VioWrtTTY ("Cannot device control block info\r\n", 34, 0);
      DosExit (EXIT_PROCESS, 1);
    }

  /* indicate receive buffer is currently empty */
  head = tail = 0;

  /* spawn receive thread */
  if (DosCreateThread (&RecvThreadID, async_isr, 0, 0, 4096))
    {
      VioWrtTTY ("Cannot create receive thread.\r\n", 31, 0);
      DosExit (EXIT_PROCESS, 1);
    }
  dtr (1);
}


/* This function closes out the com port, removing the interrupt routine,
 * etc.
 */
void closeport (void)
{
  /* kill receive thread and wait for it to close */
  DosKillThread (RecvThreadID);
  DosWaitThread (&RecvThreadID, DCWW_WAIT);

  /* close COM port handle */
  DosClose (PortHandle);
}


/* This returns the status of the carrier detect lead from the modem */
int cdet (void)
{
  BYTE instat;

  /* if DosDevIOCtl() returns an error, return 0 */
  if (DosDevIOCtl (PortHandle, IOCTL_ASYNC, ASYNC_GETMODEMINPUT,
		   NULL, 0, NULL, &instat, sizeof (instat), NULL))
    return 0;

  /* otherwise return carrier detect status */
  return (instat & DCD_ON);
}


void main (int argc, char *argv[])
{
  int done = 0;
  char ch1,
       s[3] = {' ', 7, 0},
       s1[3] = {' ', 7, 0};
  USHORT x, y;
  VIOMODEINFO vmi;
  KBDKEYINFO ki;

  /* print intro message */
  VioWrtTTY("\r\nOS2TERM v1.00  Copyright 1993 by Jeff M. Garzik\r\n"
	    "An OS/2 2.x 32-bit serial programming example\r\n"
	    "Hit Alt-X to exit, Alt-Z for list of commands\r\n"
	    "\r\n", 147, 0);

  /* check command line argument validity */
  if (argc != 2)
    {
      VioWrtTTY("Usage: OS2TERM <com-port-number>\r\n"
		"\r\n"
		"Example: OS2TERM 1\r\n", 56, 0);
      DosExit(EXIT_PROCESS, 1);
    }

  /* find out number of rows and columns on the screen */
  vmi.cb = sizeof (VIOMODEINFO);
  VioGetMode (&vmi, 0);

  /* set up the com port stuff */
  initport ((int)(argv[1][0] - '0'));
  set_baud (BAUD_RATE);

  /* init this to 0 so that KbdCharIn() does not report any false
   * character readings the first time through */
  ki.chScan = 0;

  /* the main i/o loop */
  while (!done)
    {

      /* if a character from the modem is available, process it */
      if (comhit ())
	switch (ch1 = get1c ())
	  {
	  case 0:
	  case 255: /* ignore null characters */
	    break;
	  case 12: /* formfeed char = clear screen */
	    VioScrollUp (0, 0, 65535, 65535, 65535, s, 0);
	    VioSetCurPos (0, 0, 0);
	    break;
	  case '\b': /* backspace char */
	    VioGetCurPos (&y, &x, 0);
	    if (x > 0)
	      VioSetCurPos (y, x - 1, 0);
	    break;
	  case '\r': /* carraige return */
	    VioGetCurPos (&y, &x, 0);
	    VioSetCurPos (y, 0, 0);
	    break;
	  case '\n': /* line feed */
	    VioGetCurPos (&y, &x, 0);
	    if (y == (vmi.row - 1))
	      VioScrollUp (0, 0, vmi.row - 1, vmi.col - 1, 1, s, 0);
	    else
	      VioSetCurPos (y + 1, x, 0);
	    break;
	  default: /* anything else not processed above print on screen */
	    VioGetCurPos (&y, &x, 0);
	    s1[0] = ch1;
	    VioWrtCellStr (s1, 2, y, x, 0);
	    if (x == (vmi.col - 1))
	      if (y == (vmi.row - 1))
		{
		  VioScrollUp (0, 0, vmi.row - 1, vmi.col - 1, 1, s, 0);
		  VioSetCurPos (y, 0, 0);
		}
	      else
		VioSetCurPos (y + 1, 0, 0);
	    else
	      VioSetCurPos (y, x + 1, 0);
	    break;
	  }

      /* if a character from the keyboard is available, process it */
      KbdCharIn (&ki, 1, 0);
      if (ki.chScan != 0)
	{

	  /* if chChar = 0 then it was an extended char, like F1 or Alt-X;
	   * process known extended keys */
	  if (ki.chChar == 0)
	    {
	      switch (ki.chScan)
		{
		case 0x2E:	/* alt-c - clear screen */
		  VioScrollUp (0, 0, 65535, 65535, 65535, s, 0);
		  VioSetCurPos (0, 0, 0);
		  break;
		case 0x23:	/* alt-h - drop DTR to hang up */
		  VioWrtTTY ("\r\nHanging up...", 15, 0);
		  dtr (0);
		  DosSleep (500);
		  dtr (1);
		  VioWrtTTY ("Done.\r\n", 7, 0);
		  break;
		case 0x2D:	/* alt-x - exit program */
		  VioWrtTTY ("Exiting...\r\n", 12, 0);
		  done = 1;
		  break;
		case 0x2C:	/* alt-z - print menu */
		  VioWrtTTY ("\r\n"
			     "OS2TERM Commands\r\n"
			     "----------------\r\n"
			     "Alt-C  Clear screen\r\n"
			     "Alt-H  Hang up\r\n"
			     "Alt-X  Exit OS2TERM\r\n"
			     "Alt-Z  Print list of commands\r\n"
			     "\r\n", 129, 0);
		  break;
		}
	    }

	  /* otherwise, send key to modem */
	  else
	    outcomch ((unsigned char) ki.chChar);

	  /* set scan back to 0 because when calling KbdCharIn() with
	   * IOWait parameter set to 1, KbdCharIn will exit w/o changing
	   * the data if no key is available */
	  ki.chScan = 0;
	}
    }

  /* we are exiting, shut down threads and com port */
  closeport ();

  /* return to OS/2 with successful return code */
  DosExit(EXIT_PROCESS, 0);
}

