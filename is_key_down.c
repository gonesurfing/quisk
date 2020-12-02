#include <Python.h>	// used by quisk.h
#include <complex.h>	// Used by quisk.h
#include "quisk.h"

// This module provides methods to access the state of the key.
// First call quisk_open_key(name) to choose a method and initialize.
// Subsequent key access uses the method chosen.

static int startup_error;		// error opening port
static int bit_cts, bit_dsr;		// modem bits
static char use_cts, use_dsr;		// use of CTS and DSR bits
static int reverse_cts, reverse_dsr;	// opposite polarity for cts/dsr
static char port_name[QUISK_SC_SIZE];	// serial port name
static PyObject * start_up(void);	// open the serial port
static void shut_down(void);		// close the serial port
static void modem_status(void);		// test modem bits
#define MSG_SIZE	120

//int quisk_serial_key_errors = 0;
int quisk_serial_key_down;		// The cts or dsr bit for CW key is asserted
int quisk_use_serial_port;		// either cts or dsr is being used
int quisk_serial_ptt;			// The cts or dsr bit for PTT is asserted

PyObject * quisk_open_key(PyObject * self, PyObject * args)
{  // return a message for error, or "" for no error
	PyObject * msg;
	char * port, * cts, * dsr;

	if (!PyArg_ParseTuple (args, "sss", &port, &cts, &dsr))
		return NULL;
	strncpy(port_name, port, QUISK_SC_SIZE);
	//quisk_serial_cts and dsr are "None", "CW", "PTT"; and "when high" or "when low"
	use_cts = * cts;		// 'N', 'C', 'P'
	reverse_cts = strstr(cts, "when low") != NULL;
	use_dsr = * dsr;
	reverse_dsr = strstr(dsr, "when low") != NULL;

	quisk_serial_key_down = 0;
	quisk_use_serial_port = 0;
	quisk_serial_ptt = 0;
	if (port_name[0] && (use_cts != 'N' || use_dsr != 'N')) {	// try to open the serial port
		msg = start_up();
		if ( ! startup_error)
			quisk_use_serial_port = 1;
	}
	else {
		msg = PyUnicode_FromString("");
	}
	return msg;
}

PyObject * quisk_close_key(PyObject * self, PyObject * args)
{
	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	shut_down();
	quisk_serial_key_down = 0;
	quisk_use_serial_port = 0;
	quisk_serial_ptt = 0;
	Py_INCREF (Py_None);
	return Py_None;
}

void quisk_poll_hardware_key(void)
{  // call frequently to check the modem bits
	if ( ! quisk_use_serial_port)
		return;
	modem_status();
	if (use_cts == 'C')
		quisk_serial_key_down = reverse_cts ? bit_cts == 0 : bit_cts != 0;
	else if (use_cts == 'P')
		quisk_serial_ptt = reverse_cts ? bit_cts == 0 : bit_cts != 0;
	if (use_dsr == 'C')
		quisk_serial_key_down = reverse_dsr ? bit_dsr == 0 : bit_dsr != 0;
	else if (use_dsr == 'P')
		quisk_serial_ptt = reverse_dsr ? bit_dsr == 0 : bit_dsr != 0;
}

#if defined(MS_WINDOWS)
#include <stdlib.h>
#include <sys/types.h>
#include <Windows.h>
//#include <processthreadsapi.h>
//#include <timeapi.h>
#include <avrt.h>

static HANDLE hComm = INVALID_HANDLE_VALUE;		// Windows handle to read the serial port

static void modem_status(void)
{
	DWORD dwModemStatus;

	if (hComm == INVALID_HANDLE_VALUE) {
		bit_cts = bit_dsr = 0;
	}
	else {
		if (!GetCommModemStatus(hComm, &dwModemStatus)) {
			bit_cts = bit_dsr = 0;	// Error in GetCommModemStatus;
		}
		else {
			bit_cts = MS_CTS_ON & dwModemStatus;
			bit_dsr = MS_DSR_ON & dwModemStatus;
		}
	}
}

static PyObject * start_up(void)
{
	char msg[MSG_SIZE];

	hComm = CreateFile(port_name,  GENERIC_READ, 0, 0, OPEN_EXISTING,
		0, 0);
		//FILE_FLAG_OVERLAPPED, 0);
	if (hComm == INVALID_HANDLE_VALUE) {
		snprintf(msg, MSG_SIZE, "Open Morse key serial port %s failed.", port_name);
		startup_error = 1;
		return PyUnicode_FromString(msg);
	}
	startup_error = 0;
	return PyUnicode_FromString("");
}

static void shut_down(void)
{
	if (hComm != INVALID_HANDLE_VALUE)
        	CloseHandle(hComm);
	hComm = INVALID_HANDLE_VALUE;
}

// Changes for MacOS support thanks to Mario, DL3LSM.
// Broken by N2ADR April, 2020.
#elif defined(__MACH__)

static PyObject * start_up(void)
{
	startup_error = 0;
	return PyUnicode_FromString("");
}

static void shut_down(void)
{
}

static void modem_status(void)
{
}

#else
// Not MS Windows and not __MACH__:

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>

static int fdComm = -1;			// File descriptor to read the serial port

static void modem_status(void)
{
	int bits;

        if (fdComm >= 0) {
	        ioctl(fdComm, TIOCMGET, &bits);	// read modem bits
	        bit_cts = bits & TIOCM_CTS;
	        bit_dsr = bits & TIOCM_DSR;
        }
}

static PyObject * start_up(void)
{
	int bits;
	char msg[MSG_SIZE];

	fdComm = open(port_name, O_RDWR | O_NOCTTY);
	if (fdComm < 0) {
		snprintf(msg, MSG_SIZE, "Open morse key serial port %s failed.", port_name);
		startup_error = 1;
		return PyUnicode_FromString(msg);
	}
	else {
		ioctl(fdComm, TIOCMGET, &bits);		// read modem bits
		bits |= TIOCM_DTR;			// Set DTR
		bits &= ~TIOCM_RTS;			// Clear RTS
		ioctl(fdComm, TIOCMSET, &bits);
	}
	QuiskSleepMicrosec(10000);
	startup_error = 0;
	return PyUnicode_FromString("");
}

static void shut_down(void)
{
	if (fdComm >= 0)
		close(fdComm);
	fdComm = -1;
}
#endif
