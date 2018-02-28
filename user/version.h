#ifdef MC_66B
#	define HW_MODEL_STRING "MC-B"
#elif defined(EN61107)
#	define HW_MODEL_STRING "MC"
#elif defined(IMPULSE)
#	define HW_MODEL_STRING "IMPULSE"
#elif defined(DEBUG_NO_METER)
#	define HW_MODEL_STRING "NO_METER"
#else
#	define HW_MODEL_STRING "KMP"
#endif

#ifdef FORCED_FLOW_METER
#	define FORCED_FLOW_METER_STRING "-FLOW"
#else
#	define FORCED_FLOW_METER_STRING ""
#endif	// FORCED_FLOW_METER

#if defined(IMPULSE) || defined(NO_METER)
#	define THERMO_TYPE_STRING ""
#else
#	ifdef THERMO_NO
#		define THERMO_TYPE_STRING "-THERMO_NO"
#	else	// THERMO_NC
#		define THERMO_TYPE_STRING "-THERMO_NC"
#	endif	// THERMO_NO
#endif	// IMPULSE

#ifdef THERMO_ON_AC_2
#	define THERMO_ON_AC_2_STRING "-THERMO_ON_AC_2"
#else
#	define THERMO_ON_AC_2_STRING ""
#endif	// THERMO_ON_AC_2

#ifndef AUTO_CLOSE	// reversed!
#	define AUTO_CLOSE_STRING "-NO_AUTO_CLOSE"
#else
#	define AUTO_CLOSE_STRING ""
#endif	// AUTO_CLOSE

#define HW_MODEL (HW_MODEL_STRING FORCED_FLOW_METER_STRING THERMO_TYPE_STRING THERMO_ON_AC_2_STRING AUTO_CLOSE_STRING)
