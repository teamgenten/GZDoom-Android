/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */


/* fluid_midishare.c
 *
 * Author: Stephane Letz  (letz@grame.fr)  Grame
 *
 * Interface to Grame's MidiShare drivers (www.grame.fr/MidiShare)
 * 21/12/01 : Add a compilation flag (MIDISHARE_DRIVER) for driver or application mode
 * 29/01/02 : Compilation on MacOSX, use a task for typeNote management
 * 03/06/03 : Adapdation for FluidSynth API
 * 18/03/04 : In appplication mode, connect MidiShare to the fluidsynth client (fluid_midishare_open_appl)
 */

#include "config.h"

#if MIDISHARE_SUPPORT

#include "fluid_midi.h"
#include "fluid_mdriver.h"
#include <MidiShare.h>

/* constants definitions    */
#define MidiShareDrvRef		127

#if defined(MACINTOSH) && defined(MACOS9)
#define MSHSlotName		"\pfluidsynth"
#define MSHDriverName	"\pfluidsynth"
#else
#define MSHSlotName		"fluidsynth"
#define MSHDriverName	"fluidsynth"
#endif

typedef struct {
  fluid_midi_driver_t driver;
  int status;
  short refnum;
  MidiFilterPtr filter;
  #if defined(MACINTOSH) && defined(MACOS9)
  	UPPRcvAlarmPtr upp_alarm_ptr;
  	UPPDriverPtr   upp_wakeup_ptr;
  	UPPDriverPtr   upp_sleep_ptr;
  	UPPTaskPtr     upp_task_ptr;
  #endif
  SlotRefNum	slotRef;
} fluid_midishare_midi_driver_t;


fluid_midi_driver_t* new_fluid_midishare_midi_driver(fluid_settings_t* settings,
						   handle_midi_event_func_t handler,
						   void* data);

int delete_fluid_midishare_midi_driver(fluid_midi_driver_t* p);
int fluid_midishare_midi_driver_status(fluid_midi_driver_t* p);

static void fluid_midishare_midi_driver_receive(short ref);
static int fluid_midishare_open_appl (fluid_midishare_midi_driver_t* dev);
static int fluid_midishare_open_driver (fluid_midishare_midi_driver_t* dev);
static void fluid_midishare_close_driver (fluid_midishare_midi_driver_t* dev);
static void fluid_midishare_close_appl (fluid_midishare_midi_driver_t* dev);

/*
 * new_fluid_midishare_midi_driver
 */
fluid_midi_driver_t*
new_fluid_midishare_midi_driver(fluid_settings_t* settings,
				handle_midi_event_func_t handler,
				void* data)
{
  fluid_midishare_midi_driver_t* dev;
  int i;

  /* not much use doing anything */
  if (handler == NULL) {
    FLUID_LOG(FLUID_ERR, "Invalid argument");
    return NULL;
  }

  /* allocate the device */
  dev = FLUID_NEW(fluid_midishare_midi_driver_t);
  if (dev == NULL) {
    FLUID_LOG(FLUID_ERR, "Out of memory");
    return NULL;
  }
  FLUID_MEMSET(dev, 0, sizeof(fluid_midishare_midi_driver_t));
  dev->driver.handler = handler;
  dev->driver.data = data;

  /* register to MidiShare as Application or Driver */
  #if defined(MIDISHARE_DRIVER)
  	if (!fluid_midishare_open_driver(dev)) goto error_recovery;
  #else
  	if (!fluid_midishare_open_appl(dev)) goto error_recovery;
  #endif

  /*MidiSetInfo(dev->refnum, dev->router->synth); */
  MidiSetInfo(dev->refnum, dev);
  dev->filter = MidiNewFilter();
  if (dev->filter == 0) {
    FLUID_LOG(FLUID_ERR, "Can not allocate MidiShare filter");
    goto error_recovery;
  }

  for (i = 0 ; i < 256; i++) {
  	MidiAcceptPort(dev->filter, i, 1); /* accept all ports */
  	MidiAcceptType(dev->filter, i, 0); /* reject all types */
  }

  for (i = 0 ; i < 16; i++) {
  	MidiAcceptChan(dev->filter, i, 1); /* accept all chan */
  }

  /* accept only the following types */
  MidiAcceptType(dev->filter, typeNote, 1);
  MidiAcceptType(dev->filter, typeKeyOn, 1);
  MidiAcceptType(dev->filter, typeKeyOff, 1);
  MidiAcceptType(dev->filter, typeCtrlChange, 1);
  MidiAcceptType(dev->filter, typeProgChange, 1);
  MidiAcceptType(dev->filter, typePitchWheel, 1);

  /* set the filter */
  MidiSetFilter(dev->refnum, dev->filter);

  dev->status = FLUID_MIDI_READY;
  return (fluid_midi_driver_t*) dev;

 error_recovery:
  delete_fluid_midishare_midi_driver((fluid_midi_driver_t*) dev);
  return NULL;
}

/*
 * delete_fluid_midishare_midi_driver
 */
int delete_fluid_midishare_midi_driver(fluid_midi_driver_t* p)
{
  fluid_midishare_midi_driver_t* dev;

  dev = (fluid_midishare_midi_driver_t*) p;
  if (dev == NULL) {
    return FLUID_OK;
  }

  if (dev->filter) MidiFreeFilter(dev->filter);

  #if defined(MIDISHARE_DRIVER)
  	 fluid_midishare_close_driver(dev);
  #else
  	 fluid_midishare_close_appl(dev);
  #endif

  #if defined(MACINTOSH) && defined(MACOS9)
  	DisposeRoutineDescriptor(dev->upp_alarm_ptr);
  	DisposeRoutineDescriptor(dev->upp_wakeup_ptr);
  	DisposeRoutineDescriptor(dev->upp_sleep_ptr);
  	DisposeRoutineDescriptor(dev->upp_task_ptr);
  #endif

  dev->status = FLUID_MIDI_DONE;

  FLUID_FREE(dev);
  return FLUID_OK;
}


/*
 * fluid_midishare_midi_driver_status
 */
int fluid_midishare_midi_driver_status(fluid_midi_driver_t* p)
{
  fluid_midishare_midi_driver_t* dev = (fluid_midishare_midi_driver_t*) p;
  return dev->status;
}


/*
 * fluid_midishare_keyoff_task
 */
static void fluid_midishare_keyoff_task (long date, short ref, long a1, long a2, long a3)
{
	fluid_midishare_midi_driver_t* dev = (fluid_midishare_midi_driver_t*)MidiGetInfo(ref);
	fluid_midi_event_t new_event;
  	MidiEvPtr e =(MidiEvPtr)a1;

	fluid_midi_event_set_type(&new_event, NOTE_OFF);
	fluid_midi_event_set_channel(&new_event, Chan(e));
	fluid_midi_event_set_pitch(&new_event, Pitch(e));
	fluid_midi_event_set_velocity(&new_event, Vel(e)); /* release vel */

	/* and send it on its way to the router */
	(*dev->driver.handler)(dev->driver.data, &new_event);

	MidiFreeEv(e);
}


/*
 * fluid_midishare_midi_driver_receive
 */
static void fluid_midishare_midi_driver_receive(short ref)
{
	fluid_midishare_midi_driver_t* dev = (fluid_midishare_midi_driver_t*)MidiGetInfo(ref);
	fluid_midi_event_t new_event;
	MidiEvPtr e;

	while ((e = MidiGetEv(ref))){

	    switch (EvType (e)){

		case typeNote:
		      /* Copy the data to fluid_midi_event_t */
		      fluid_midi_event_set_type(&new_event, NOTE_ON);
		      fluid_midi_event_set_channel(&new_event, Chan(e));
		      fluid_midi_event_set_pitch(&new_event, Pitch(e));
		      fluid_midi_event_set_velocity(&new_event, Vel(e));

		      /* and send it on its way to the router */
		      (*dev->driver.handler)(dev->driver.data, &new_event);

		      #if defined(MACINTOSH) && defined(MACOS9)
		      	MidiTask(dev->upp_task_ptr, MidiGetTime()+Dur(e), ref, (long)e, 0, 0);
		      #else
		      	MidiTask(fluid_midishare_keyoff_task, MidiGetTime()+Dur(e), ref, (long)e, 0, 0);
		      #endif
		      break;

		    case typeKeyOn:
		      /* Copy the data to fluid_midi_event_t */
		      fluid_midi_event_set_type(&new_event, NOTE_ON);
		      fluid_midi_event_set_channel(&new_event, Chan(e));
		      fluid_midi_event_set_pitch(&new_event, Pitch(e));
		      fluid_midi_event_set_velocity(&new_event, Vel(e));

		      /* and send it on its way to the router */
		      (*dev->driver.handler)(dev->driver.data, &new_event);

		      MidiFreeEv(e);
		      break;

		    case typeKeyOff:
		      /* Copy the data to fluid_midi_event_t */
		      fluid_midi_event_set_type(&new_event, NOTE_OFF);
		      fluid_midi_event_set_channel(&new_event, Chan(e));
		      fluid_midi_event_set_pitch(&new_event, Pitch(e));
		      fluid_midi_event_set_velocity(&new_event, Vel(e)); /* release vel */

		      /* and send it on its way to the router */
		      (*dev->driver.handler)(dev->driver.data, &new_event);

		      MidiFreeEv(e);
		      break;

		    case typeCtrlChange:
		      /* Copy the data to fluid_midi_event_t */
		      fluid_midi_event_set_type(&new_event, CONTROL_CHANGE);
		      fluid_midi_event_set_channel(&new_event, Chan(e));
		      fluid_midi_event_set_control(&new_event, MidiGetField(e,0));
		      fluid_midi_event_set_value(&new_event, MidiGetField(e,1));

		      /* and send it on its way to the router */
		      (*dev->driver.handler)(dev->driver.data, &new_event);

		      MidiFreeEv(e);
		      break;

		    case typeProgChange:
		      /* Copy the data to fluid_midi_event_t */
		      fluid_midi_event_set_type(&new_event, PROGRAM_CHANGE);
		      fluid_midi_event_set_channel(&new_event, Chan(e));
		      fluid_midi_event_set_program(&new_event, MidiGetField(e,0));

		      /* and send it on its way to the router */
		      (*dev->driver.handler)(dev->driver.data, &new_event);

		      MidiFreeEv(e);
		      break;

		    case typePitchWheel:
		      /* Copy the data to fluid_midi_event_t */
		      fluid_midi_event_set_type(&new_event, PITCH_BEND);
		      fluid_midi_event_set_channel(&new_event, Chan(e));
		      fluid_midi_event_set_value(&new_event, ((MidiGetField(e,0)
							      + (MidiGetField(e,1) << 7))
							      - 8192));

		      /* and send it on its way to the router */
		      (*dev->driver.handler)(dev->driver.data, &new_event);

		      MidiFreeEv(e);
		      break;
		 }
	  }
}


/*
 * fluid_midishare_wakeup
 */
 static void fluid_midishare_wakeup (short r)
{
	MidiConnect (MidiShareDrvRef, r, true);
	MidiConnect (r, MidiShareDrvRef, true);
}

/*
 * fluid_midishare_sleep
 */
 static void fluid_midishare_sleep (short r){}

/*
 * fluid_midishare_open_driver
 */
static int fluid_midishare_open_driver (fluid_midishare_midi_driver_t* dev)
{
	/* gcc wanted me to use {0,0} to initialize the reserved[2] fields */
	TDriverInfos infos = { MSHDriverName, 100, 0, { 0, 0 } };
	TDriverOperation op = { fluid_midishare_wakeup, fluid_midishare_sleep, { 0, 0, 0 } };

	/* register to MidiShare */
	#if defined(MACINTOSH) && defined(MACOS9)
		dev->upp_wakeup_ptr = NewDriverPtr(fluid_midishare_wakeup);
		dev->upp_sleep_ptr = NewDriverPtr(fluid_midishare_sleep);

		op.wakeup = (WakeupPtr)dev->upp_wakeup_ptr;
		op.sleep = (SleepPtr)dev->upp_sleep_ptr;

		dev->refnum = MidiRegisterDriver(&infos, &op);
		if (dev->refnum < 0) {
			FLUID_LOG(FLUID_ERR, "Can not open MidiShare Application client");
			return 0;
		}
		dev->slotRef = MidiAddSlot (dev->refnum, MSHSlotName, MidiOutputSlot);
		dev->upp_alarm_ptr = NewRcvAlarmPtr(fluid_midishare_midi_driver_receive);
		dev->upp_task_ptr = NewTaskPtr(fluid_midishare_keyoff_task);
		MidiSetRcvAlarm(dev->refnum, dev->upp_alarm_ptr);
	#else
		dev->refnum = MidiRegisterDriver(&infos, &op);
		if (dev->refnum < 0) {
			FLUID_LOG(FLUID_ERR, "Can not open MidiShare Application client");
			return 0;
		}
		dev->slotRef = MidiAddSlot (dev->refnum, MSHSlotName, MidiOutputSlot);
		MidiSetRcvAlarm(dev->refnum, fluid_midishare_midi_driver_receive);
	#endif
	return 1;
}

/*
 * fluid_midishare_close_driver
 */
static void fluid_midishare_close_driver (fluid_midishare_midi_driver_t* dev)
{
	if (dev->refnum > 0) MidiUnregisterDriver(dev->refnum);
}

/*
 * fluid_midishare_open_appl
 */
static int fluid_midishare_open_appl (fluid_midishare_midi_driver_t* dev)
{
	/* register to MidiShare */
	#if defined(MACINTOSH) && defined(MACOS9)
		dev->refnum = MidiOpen(MSHDriverName);
		if (dev->refnum < 0) {
			FLUID_LOG(FLUID_ERR, "Can not open MidiShare Driver client");
			return 0;
		}
		dev->upp_alarm_ptr = NewRcvAlarmPtr(fluid_midishare_midi_driver_receive);
		dev->upp_task_ptr = NewTaskPtr(fluid_midishare_keyoff_task);
		MidiSetRcvAlarm(dev->refnum, dev->upp_alarm_ptr);
	#else
		dev->refnum = MidiOpen(MSHDriverName);
		if (dev->refnum < 0) {
			FLUID_LOG(FLUID_ERR, "Can not open MidiShare Driver client");
			return 0;
		}
		MidiSetRcvAlarm(dev->refnum, fluid_midishare_midi_driver_receive);
		MidiConnect(0,dev->refnum,true);
	#endif
  	return 1;
}

/*
 * fluid_midishare_close_appl
 */
static void fluid_midishare_close_appl (fluid_midishare_midi_driver_t* dev)
{
	if (dev->refnum > 0) MidiClose(dev->refnum);
}

#endif /* MIDISHARE_SUPPORT */
