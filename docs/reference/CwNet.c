// File:  C:\cbproj\Remote_CW_Keyer\CwNet.c
// Date:  2024-01-01
// Author:  Wolfgang Buescher (DL4YHF)
// Purpose: Socket-based Client or Server for the 'Remote CW Keyer'.
//          Initially just an experiment for truly "remote" CW keying,
//          but with provisions to multiplex more than just the
//          'Morse keying' pattern over a single TCP/IP connection.
//
//  The SERVER part  [ServerThread()] is losely based on two articles about
//      "Asynchronous socket programming", aka "select()-based event handling",
//      locally saved as "Socket-based async multi-client tcp server.txt" .
//  The CLIENT part  [ClientThread()] only uses a single socket, and runs
//      in a loop that also includes CONNECTING the remote server until success.
//      So it doesn't matter which of the two (server,client) is launched first.
//  Most of the application-specific processing happens in subroutines called
//      from the above worker threads. Some of those subroutines are used
//      for BOTH ENDS of a "socket connection" (client and server), e.g.:
//  * CwNet_OnReceive()  : Called whenever data arrived on a socket.
//                             Implements a simple 'streaming parser'
//                             to demultiplex the received bytestream.
//  * CwNet_ExecuteCmd() : Called from CwNet_OnReceive() when a
//                             received command is "ready for processing".
//  * CwNet_OnPoll()     : Called every ~~20 ms for any open socket,
//                             even if there was NOTHING received, to allow
//                             assembling all that the client or server needs
//                             to send in a SINGLE call of the send() function.
//                             Also performs some housekeeping, e.g. handing over
//                             'the key' (or maybe the microphone) from one
//                             user to the other.
//  * CwServer_OnConnect(), CwServer_OnDisconnect(), etc: Speak for themselves.

#include "switches.h" // project specific 'compilation switches' like SWI_USE_DSOUND

#include <string.h> // not only string functions in here, but also memset()
#include <stdio.h>  // no standard I/O but string functions like sprintf()
#ifdef _WINDOWS     // obviously compiling for a PC, so ..
# include "winsock2.h"  // .. use Microsoft's flavour of the Berkeley Socket API
  // Note: never include winsock2.h *after* windows.h .. you'll fry in hell,
  //       because "windows.h" #includes "winsock.h", not "winsock2.h" .
#endif

#include "StringLib.h" // DL4YHF's string library, with stuff like SL_ParseIPv4Address()
#include "Utilities.h" // stuff like UTL_iWindowsVersion, UTL_iAppInstance, ShowError(), etc
#include "Inet_Tools.h" // stuff like INET_DumpTraffic_HexOrASCII(), etc
#include "Timers.h"    // high-resolution 'current time'-function, T_TIM_Stopwatch, etc.
#include "Elbug.h"     // the basic 'Elbug' functions (plain C)
#include "SoundTab.h"  // cosine lookup table, filter coefficients, T_Float, etc.
#include "SampleRateConv.h"  // convert from 48 to 8 kSamples/sec and back,
                       // for 'audio streaming' between server and client[s].
#include "ALawCompression.h" // simple A-Law compression / decompression
                       // to stream audio between server and client[s].
#include "CwNet.h"     // header for THIS module ("Network-capable remote CW keying")
#if( SWI_USE_HTTP_SERVER ) // build with an integrated HTTP server ?
# include "HttpServer.h"  // experimental, specialized HTTP server (NOT a "HTML file server")
#endif // SWI_USE_HTTP_SERVER ?

#ifndef HAMLIB_RESULT_NO_ERROR
# include "HamlibResultCodes.h" // Integer values returned by 'rigctld'-compatible command handlers,
                                // NEGATIVE values indicate various ERRORS,  ZERO means "No Error".
#endif // ndef HAMLIB_RESULT_NO_ERROR ?

#if( SWI_NUM_AUX_COM_PORTS > 0 ) // compile with support for 'Auxiliary / Additional COM ports' / Winkeyer emulation ?
  //  '--> NUMBER OF "auxiliary"/"additional" COM ports, defined in switches.h
# include "AuxComPorts.h" // structs and API functions for the "Auxiliary" (later: "Additional") COM ports
  // (Module CwNet.c accesses the thread-safe circular FIFOs in AuxComPorts[SWI_NUM_AUX_COM_PORTS]
  //  to implement the TCP/IP side in Auxiliary-COM-port-mode RIGCTRL_PORT_USAGE_SERIAL_TUNNEL )
#endif // SWI_NUM_AUX_COM_PORTS ?



//----------------------------------------------------------------------------
// "Internal" constants, macros, lookup tables, and similar
//----------------------------------------------------------------------------

#define CWNET_SOCKET_POLLING_INTERVAL_MS 20 // interval, in milliseconds,
                       // at which all of the active client <-> server sockets
                       // are PERIODICALLY POLLED for transmission, even if
                       // nothing has been received for a while.

#ifndef  sizeof_member
# define sizeof_member(type,member) sizeof(((type*)0)->member) /* ugly.. but works */
#endif

#if( ! SWI_USE_HTTP_SERVER ) // if HttpServer.c isn't included in the project, use THESE token:
#define HTTP_METHOD_UNKNOWN 0
#define HTTP_METHOD_GET     1
#define HTTP_METHOD_POST    2
#define HTTP_METHOD_PUT     3
#define HTTP_METHOD_DELETE  4
#define HTTP_METHOD_OPTIONS 5
#define HTTP_METHOD_HEAD    6
const T_SL_TokenList HttpMethods[] =
{ { "GET ",    HTTP_METHOD_GET    },
  { "POST ",   HTTP_METHOD_POST   },
  { "PUT ",    HTTP_METHOD_PUT    },
  { "DELETE ", HTTP_METHOD_DELETE },
  { "OPTIONS ",HTTP_METHOD_OPTIONS}, // <-- added 2015-12-08 to make a paranoid web browser happy
  { "HEAD ",   HTTP_METHOD_HEAD   }, // "HEAD" is like "GET" without a body in the response:
  { NULL, 0 } // "all zeros" mark the end of the list
};
#endif // ! SWI_USE_HTTP_SERVER ?


//----------------------------------------------------------------------------
// Global variables for hardcore debugging / post-mortem crash analysis,
//        when the debugger's call stack shows nothing but garbage,
//        e.g. after an exception at the infamous address 0xFEEEFEEE .
//        ( C++Builder's debugger could still inspect SIMPLE GLOBAL
//          VARIABLES after most kinds of exceptions, but of course nothing
//          stack-based because the CPU- or task-stack was usually trashed.)
//----------------------------------------------------------------------------
#if(SWI_HARDCORE_DEBUGGING) // (1) = hardcore-debugging, (0)=normal compilation
 int CwNet_iLastSourceLine = 0;  // WATCH THIS after crashing with e.g. "0xFEEEFEEE" !
# define HERE_I_AM__CWNET() CwNet_iLastSourceLine=__LINE__
     // (see complete list of other XYZ_iLastSourceLine variables to watch
     //  in C:\cbproj\Remote_CW_Keyer\Keyer_Main.cpp, near GUI_iLastSourceLine)
#else
# define HERE_I_AM__CWNET()
#endif // SWI_HARDCORE_DEBUGGING ?


//----------------------------------------------------------------------------
// "Internal function prototypes" (functions called before their implementation)
//----------------------------------------------------------------------------

static DWORD WINAPI ClientThread( LPVOID lpParam );
static DWORD WINAPI ServerThread( LPVOID lpParam );

// implementation of functions:

//----------------------------------------------------------------------------
void CwNet_InitInstanceWithDefaults( T_CwNet *pCwNet )                  // API
  // Called from the main application (or "GUI") before loading an actual
  // configuration FROM A FILE. The result (in *pCwNet) are the "defaults".
{
  int i;

  HERE_I_AM__CWNET(); // -> CwNet_iLastSourceLine (optional, for hardcore debugging)

  memset( pCwNet, 0, sizeof(T_CwNet) ); // <- this also clears various FIFOs, and whatever will be added in future
  HERE_I_AM__CWNET();
  pCwNet->dwMagicPI_1    = pCwNet->dwMagicPI_2 = pCwNet->dwMagicPI_3
   = pCwNet->dwMagicPI_4 = pCwNet->dwMagicPI_5 = pCwNet->dwMagicPI_6
   = CWNET_MAGIC_PI; // sanity check at various offsets in this struct
  for( i=0; i<=CWNET_MAX_CLIENTS/*!*/; ++i )
   { pCwNet->Client[i].dwMagicPI = CWNET_MAGIC_PI;
     // initialize the lock-free, thread-safe FIFOs between worker thread and the "Chatbox"-GUI :
     CFIFO_Init( &pCwNet->Client[i].sChatRxFifo.fifo, CWNET_CHAT_FIFO_SIZE, sizeof(BYTE), 0.0/*dblSecondsPerSample*/ );
     CFIFO_Init( &pCwNet->Client[i].sChatTxFifo.fifo, CWNET_CHAT_FIFO_SIZE, sizeof(BYTE), 0.0/*dblSecondsPerSample*/ );
   }
  pCwNet->iSetPTTFromNetwork = RIGCTRL_NOVALUE_INT;  // no "set_ptt" command received yet

  // Provide a few 'meaningful defaults'. When first launching the application,
  // these will be displayed on the 'Network' tab, because Keyer_Main.cpp
  // (or whatever the GUI is written in) will use these values as DEFAULTS
  // in a function like LoadSettings() :
  pCwNet->cfg.iFunctionality   = CWNET_FUNC_OFF; // per default, neither CWNET_FUNC_CLIENT nor CWNET_FUNC_SERVER !
  pCwNet->cfg.iDiagnosticFlags = CWNET_DIAG_FLAGS_SHOW_DECODER_OUTPUT; // per default, show output from the 'Morse decoder' somewhere ("Debug"-tab?)
  strcpy( pCwNet->cfg.sz80ClientRemoteIP, "localhost:7355" ); // <- should be obvious that this is "something LOCAL"
  strcpy( pCwNet->cfg.sz80ClientUserName, "Max" );            // <- should be obvious that this is "just a dummy"
  strcpy( pCwNet->cfg.sz80ClientUserName, "" );               // <- SWLs please leave this field EMPTY (don't transmit with e.g. "NoCall")
  pCwNet->cfg.iServerListeningPort = 7355;   // <- obviously the CLIENT tries to connect the above 'amateur radio port'
  strcpy( pCwNet->cfg.sz255AcceptUsers, "Max:0,Moritz:7" ); // <- obviously a comma-separated list of user names
  pCwNet->cfg.iNetworkLatency_ms = 250; // fixed network latency in milliseconds (0 would be "auto-detect")
#if( SWI_USE_HTTP_SERVER )  // build with an integrated HTTP server ?
  pCwNet->cfg.iHttpServerOptions = HTTP_SERVER_OPTIONS_ENABLE | HTTP_SERVER_OPTIONS_RESTRICT; // defaults; see Remote_CW_Keyer/HttpServer.h .
#endif // SWI_USE_HTTP_SERVER ?
  HERE_I_AM__CWNET();

} // end CwNet_InitInstanceWithDefaults()

//----------------------------------------------------------------------------
int CwNet_SanityCheck( T_CwNet *pCwNet )                               // API
  // Returns ZERO when no error was found, or some nonzero/negative
  //   "phantasy error code" that only means something for the developer.
{
  int i;
  // Not so efficient, but debug-friendly:
  if( pCwNet->dwMagicPI_1 != CWNET_MAGIC_PI )
   { pCwNet->dwMagicPI_1 = CWNET_MAGIC_PI;   // prevent detecting the same error twice
     return -1;
   }
  if( pCwNet->dwMagicPI_2 != CWNET_MAGIC_PI )
   { pCwNet->dwMagicPI_2 = CWNET_MAGIC_PI;
     return -2;
   }
  if( pCwNet->dwMagicPI_3 != CWNET_MAGIC_PI )
   { pCwNet->dwMagicPI_3 = CWNET_MAGIC_PI;
     return -3;
   }
  if( pCwNet->dwMagicPI_4 != CWNET_MAGIC_PI )
   { pCwNet->dwMagicPI_4 = CWNET_MAGIC_PI;
     return -4;
   }
  if( pCwNet->dwMagicPI_5 != CWNET_MAGIC_PI )
   { pCwNet->dwMagicPI_5 = CWNET_MAGIC_PI;
     return -5;
   }
  if( pCwNet->dwMagicPI_6 != CWNET_MAGIC_PI )
   { pCwNet->dwMagicPI_6 = CWNET_MAGIC_PI;
     return -6;
   }
  for( i=0; i<=CWNET_MAX_CLIENTS/*!*/; ++i )
   { if( pCwNet->Client[i].dwMagicPI != CWNET_MAGIC_PI )
      {  pCwNet->Client[i].dwMagicPI = CWNET_MAGIC_PI;
         return -7 - i;
      }
   }

  return 0;  // zero errors during the 'struct sanity check'
} // end CwNet_SanityCheck()

//----------------------------------------------------------------------------
long CwNet_ReadNonAlignedInt32( BYTE *pbSource )
{ return (long)pbSource[0] // INTEL byte order .. least significant byte FIRST
      | ((long)pbSource[1] << 8)
      | ((long)pbSource[2] << 16)
      | ((long)pbSource[3] << 24);
} // end CwNet_ReadNonAlignedInt32()

//----------------------------------------------------------------------------
BOOL CwNet_Start( T_CwNet *pCwNet )                                     // API
  // ,---------------------'
  // '-- [in] usually the single instance from Keyer_Main.cpp ( &MyCwNet ) .
{
  int i;

  HERE_I_AM__CWNET();
  CwNet_Stop( pCwNet );  // <- doesn't do harm when NOT running at all
  pCwNet->dwThreadLoops  = pCwNet->dwThreadErrors = 0;
  pCwNet->dwNumBytesSent = pCwNet->dwNumBytesRcvd = 0;
  pCwNet->iTransmittingClient = -1;   // anyone may "take the key" now ..
  pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX].iAnnouncedTxClient = pCwNet->iTransmittingClient;
  TIM_StartStopwatch( &pCwNet->swTransmittingClient );
     // ,----------------------'
     // '-> this will be controlled by CwNet_SwitchTransmittingClient() later.
  // Reset the states of all 'client instance', including the 'local client',
  // to avoid confusing displays on the DEBUG tab like
  //   > Client #0 : state = login (when iFunctionality is NOT CWNET_FUNC_CLIENT)
  for( i=0; i<=/*!*/CWNET_MAX_CLIENTS; ++i ) // note: i=0 is the "local client"
   { pCwNet->Client[i].iClientState = CWNET_CLIENT_STATE_DISCONN;
   }

#if( SWI_USE_VORBIS_STREAM ) // allow streaming audio via Ogg/Vorbis ? [OPTIONAL]
  HERE_I_AM__CWNET();
  VorbisStream_Init( &pCwNet->sVorbis ); // <- 2024-02-25: Trashed MyDirectSound.sz255OutputDeviceName ?!
  HERE_I_AM__CWNET();
  if( ! VorbisStream_InitEncoder( &pCwNet->sVorbis,
      8000.0f, // [in] fltSampleRate: number of sample points per second
      1,       // [in] nChannelsPerSample: number of audio channels PER SAMPLE POINT
      0.8f) )  // [in] base_quality: quality level, ranging from -0.1 to 1.0 (lo to hi).
               //      (with base_quality=0.4 and f_sample = 8 kHz,
               //       MANY SECONDS elapsed between the emission of two Ogg pages
               //       to the stream - totally unsuitable for 'low latency web streams'.
               // 2024-02-14: Increased base_quality from 0.4 to 0.8
               // -> Ogg/Vorbis encoder:  192 kSamples encoded, 8 kS/s, 11 Ogg pages, ..
               //      (that's only 11 "pages" after 192/8= 24 s, i.e. 0.45 pages/s)
   { HERE_I_AM__CWNET();
     ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "VorbisEncoder: %s",
                 pCwNet->sVorbis.sz255ErrorString );
   }
  // If all works as planned, any remote client asking for 'Vorbis audio in Ogg pages'
  //       may receive the three(?) 'initial headers' delivered via
  //       VorbisStream_GetInitialHeadersFromEncoder() now.
  // The actual Vorbis *ENCODING* process won't start before anyone really
  // ASKS for an Ogg/Vorbis-encoded audio stream, indicated by
  // T_CwNetClient.fWantVorbis (TRUE="wants Ogg/Vorbis",
  //                            FALSE="expects 8-bit A-Law compressed audio")
#endif // SWI_USE_VORBIS_STREAM ?

  if( (pCwNet->cfg.iFunctionality == CWNET_FUNC_CLIENT)   // only ONE of these..
    ||(pCwNet->cfg.iFunctionality == CWNET_FUNC_SERVER) ) // .. may actually run
   {
     // Create a worker thread for either the TCP client or -server, the Win32 way:
     if( pCwNet->hThread==NULL )
      {  pCwNet->iThreadStatus = CWNET_THREAD_STATUS_LAUNCHED; // "just launched, soon RUNNING"
         HERE_I_AM__CWNET();
         pCwNet->hThread = CreateThread(
            NULL,    // LPSECURITY_ATTRIBUTES lpThreadAttributes = pointer to thread security attributes
            2*65536, // DWORD dwStackSize  = initial thread stack size, in bytes
            (pCwNet->cfg.iFunctionality == CWNET_FUNC_CLIENT) // start WHICH of the two thread functions ?
             ? ClientThread // LPTHREAD_START_ROUTINE lpStartAddress = pointer to thread function ..
             : ServerThread,
            (LPVOID)pCwNet, // LPVOID lpParameter = argument for new thread
            0,       // DWORD dwCreationFlags = creation flags
                     // zero -> the thread runs immediately after creation
            &pCwNet->dwThreadId // LPDWORD lpThreadId = pointer to returned thread id
            // > If CreateThread() fails, the return value is NULL (not INVALID_HANDLE_VALUE) .
            );
        HERE_I_AM__CWNET();
        // > The thread object remains in the system until the thread has terminated
        // > and all handles to it have been closed through a call to CloseHandle.
     } // end if < CLIENT or SERVER enabled >  ?
    if( pCwNet->hThread==NULL ) // Check the return value for success.
     {
       pCwNet->pszLastError  = "CreateThread failed.";
       pCwNet->iThreadStatus = CWNET_THREAD_STATUS_NOT_CREATED;
       CwNet_Stop(pCwNet); // here: set various struct members back to defaults
       HERE_I_AM__CWNET();
       return FALSE;
     }

    // > Define the Thread's priority as required.
    SetThreadPriority( pCwNet->hThread, // handle to the thread
               THREAD_PRIORITY_NORMAL); // thread priority level
    HERE_I_AM__CWNET();
    return TRUE;

   } // end if < iFunctionality == CWNET_FUNC_CLIENT or CWNET_FUNC_SERVER > ?

  // Arrived here ? There's nothing to "start", so :
  HERE_I_AM__CWNET();
  return FALSE;

} // end CwNet_Start()

//----------------------------------------------------------------------------
void CwNet_Stop( T_CwNet *pCwNet )                                      // API
{
  int i;
  if(  (pCwNet->iThreadStatus == CWNET_THREAD_STATUS_LAUNCHED )
    || (pCwNet->iThreadStatus == CWNET_THREAD_STATUS_RUNNING  ) )
   { pCwNet->iThreadStatus = CWNET_THREAD_STATUS_TERMINATE; // politely ask the thread to terminate itself
     for(i=0; i<20; ++i )
      { Sleep(10);
        if( pCwNet->iThreadStatus == CWNET_THREAD_STATUS_TERMINATED ) // bingo..
         { break;  // .. the thread has terminated itself, so no need to kill it
         }
      }
   }
  if( pCwNet->hThread != NULL )
   { CloseHandle( pCwNet->hThread );
     pCwNet->hThread  =  NULL;
   }
  pCwNet->iThreadStatus = CWNET_THREAD_STATUS_NOT_CREATED;

#if( SWI_USE_VORBIS_STREAM ) // allow streaming audio via Ogg/Vorbis ?
  VorbisStream_Exit( &pCwNet->sVorbis );
#endif // SWI_USE_VORBIS_STREAM ?


} // end CwNet_Stop()


//---------------------------------------------------------------------------
CPROT BOOL CwNet_SwitchTransmittingClient( T_CwNet *pCwNet, int iNewTransmittingClient )
  // Beware: This API function may be called from the KEYER THREAD,
  //         for example if the server's "sysop" (identified by
  //         iNewTransmittingClient == CWNET_LOCAL_CLIENT_INDEX)
  //         starts keying himself, or starts playing a message from memory.
  // Return value : TRUE when iNewTransmittingClient "has the key"
  //                     (either before or after the call)
  //                FALSE when iNewTransmittingClient "didn't get the key",
  //                      for example because another client, or the server's
  //                      sysop STILL HAS the key, and the no-activity-timer
  //                      hasn't expired yet. In that case, JUST WAIT PATIENTLY
  //                      until the currently sending client passes on the key,
  //                      or even the 'microphone' (if we ever support 'voice').
  // See also / "related functions" : RigCtrl_SetTransmitRequest(), CwNet_SwitchTransmittingClient(), .
  //   
{
  if( iNewTransmittingClient == pCwNet->iTransmittingClient )
   { return TRUE;  // "you already HAVE the key, dear OM, so just carry on"
   }
  if( iNewTransmittingClient == CWNET_LOCAL_CLIENT_INDEX ) // hello sysop..
   { pCwNet->iTransmittingClient = iNewTransmittingClient; // you've got the key..
     TIM_StartStopwatch( &pCwNet->swTransmittingClient ); // <- thread safe !
     pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX].iAnnouncedTxClient = pCwNet->iTransmittingClient;
     return TRUE;
   }
  // Arrived here: a CLIENT has asked for the key, and the key/microphone isn't his yet.
  //               Let him take it ONLY if the previous owner has dropped (released) it.
  if( pCwNet->iTransmittingClient < 0 ) // neither the sysop nor another client has the key ->
   { pCwNet->iTransmittingClient = iNewTransmittingClient; // give him the key..
     TIM_StartStopwatch( &pCwNet->swTransmittingClient ); // <- thread safe !
     pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX].iAnnouncedTxClient = pCwNet->iTransmittingClient;
     return TRUE;
   }
  // Arrived here: The key is still "occupied" by somebody else,
  //               so wait patiently until the other guy stops using it:
  if( TIM_ReadStopwatch_ms( &pCwNet->swTransmittingClient ) > 1000 ) // <- thread safe !
   { pCwNet->iTransmittingClient = iNewTransmittingClient; // allow 'breaking in' after a second (?)
     TIM_StartStopwatch( &pCwNet->swTransmittingClient ); // <- thread safe !
     pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX].iAnnouncedTxClient = pCwNet->iTransmittingClient;
     return TRUE;
   }
  pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX].iAnnouncedTxClient = pCwNet->iTransmittingClient;
  return FALSE;  // wait patiently until the other guy stops keying/talking :)

} // end CwNet_SwitchTransmittingClient()

//---------------------------------------------------------------------------
int CwNet_GetLatencyForRemoteClient_ms( T_CwNet *pCwNet, int iClient )
  // The KEYER THREAD needs to know the remote client's individual network
  // latency, to avoid drop-outs in the keying signal for the radio .
  // As the name suffix implies, the return value is in MILLISECONDS .
  // For display in the GUI (in menu 'Settings' .. 'Network Latency'),
  //   the instance running as CLIENT also knows the measured latency.
{
  int iResult = 0;
  if( (iClient>=0) && (iClient <=/*!*/ CWNET_MAX_CLIENTS) )
   { iResult = pCwNet->Client[iClient].iPingLatency_ms;
     // ,------------------------------|_____________|
     // '-->
   }
  return iResult;
} // end CwNet_GetLatencyForRemoteClient_ms()

//---------------------------------------------------------------------------
int CwNet_GetIndexOfCurrentlyActiveClient( T_CwNet *pCwNet )
{
  // If this Cw Keyer Instance is running as CLIENT or SERVER ?
  if( pCwNet->cfg.iFunctionality == CWNET_FUNC_SERVER )
   { // If any of the server's remote clients is currently TRANSMITTING,
     // return the index of that client:
     if( pCwNet->iTransmittingClient >= 0 )
      { return pCwNet->iTransmittingClient;
      }
     else // none of the clients currently "has the key"
      {   //  so let the GUI show info about THE FIRST remote client:
        return CWNET_FIRST_REMOTE_CLIENT_INDEX;
      }
   }
  else // not running as SERVER but CLIENT ->
   { return CWNET_LOCAL_CLIENT_INDEX;
   }

} // end CwNet_GetIndexOfCurrentlyActiveClient()


//---------------------------------------------------------------------------
char *CwNet_GetClientCallOrInfo( T_CwNet *pCwNet, int iClientIndex ) // .. for the GUI
{
  static char sz80DummyCall[84];
  if( iClientIndex == CWNET_LOCAL_CLIENT_INDEX )
   { return "The Sysop";
   }
  if( (iClientIndex>=CWNET_FIRST_REMOTE_CLIENT_INDEX) &&(iClientIndex <= CWNET_MAX_CLIENTS) )
   { // No-No : return pCwNet->Client[iClientIndex].sz80UserName;
     // Don't reveal USER NAMES to *other users* (keep them secret for the log-in).
     if( pCwNet->Client[iClientIndex].sz80Callsign[0] <= ' ' )
      { sprintf( sz80DummyCall, "NoCall #%d", (int)iClientIndex );
        return sz80DummyCall;
      }
     else
      { return pCwNet->Client[iClientIndex].sz80Callsign;
      }
   }
  else // as in pCwNet->iTransmittingClient, a negative value means "nobody has the key"
   { return "-- nobody --";
   }
} // end CwNet_GetClientCallOrInfo()

//---------------------------------------------------------------------------
const char* CwNet_CommandFromBytestreamToString( BYTE bCommand )
{ static char sz7Unknown[8];
  switch( bCommand & CWNET_CMD_MASK_COMMAND )
   { //               '-->  mask to strip 'the received command itself'
     // (0x3F; we ignore the two "block-length indicator bits" here)
     case CWNET_CMD_NONE     : return "NONE"; // dummy command to send HTTP instead of our binary protocol
     case CWNET_CMD_CONNECT  : return "Conn";
     case CWNET_CMD_DISCONN  : return "Disconn";
     case CWNET_CMD_PING     : return "Ping";
     case CWNET_CMD_PRINT    : return "Print";
     case CWNET_CMD_TX_INFO  : return "TX-Inf";
     case CWNET_CMD_RIGCTLD  : return "RigCtlD";
     case CWNET_CMD_MORSE    : return "Morse";
     case CWNET_CMD_AUDIO    : return "Audio";
     case CWNET_CMD_VORBIS   : return "Vorbis";
     case CWNET_CMD_RSV13    : return "Rsv13";
     case CWNET_CMD_CI_V     : return "CI-V";
     case CWNET_CMD_SPECTRUM : return "Spectrum";
     case CWNET_CMD_FREQ_REPORT  : return "FreqRpt";
     case CWNET_CMD_PARAM_INTEGER: return "IntPara";
     case CWNET_CMD_PARAM_DOUBLE : return "DblPara";
     case CWNET_CMD_PARAM_STRING : return "StrPara";
     case CWNET_CMD_QUERY_PARAM  : return "QueryPar";
     case CWNET_CMD_MULTI_FUNCTION_METER_REPORT : return "MeterRpt";
     case CWNET_CMD_POTI_REPORT  : return "PotiRpt";
     case CWNET_CMD_BAND_STACKING_REGISTER : return "BandStack";
     case CWNET_CMD_USER_DEFINED_BAND      : return "UserBand";
     case CWNET_CMD_SERIAL_PORT_TUNNEL_ANY : return "Tunnel0";
     case CWNET_CMD_SERIAL_PORT_TUNNEL_1   : return "Tunnel1";
     case CWNET_CMD_SERIAL_PORT_TUNNEL_2   : return "Tunnel2";
     case CWNET_CMD_SERIAL_PORT_TUNNEL_3   : return "Tunnel3";
     default :
        sprintf( sz7Unknown, "0x%02X", (unsigned int) bCommand );
        break;
   }
  return sz7Unknown; // don't panic for being not thread-safe .. only for DEBUGGING !
} // end CwNet_CommandFromBytestreamToString()


//---------------------------------------------------------------------------
char* CwNet_ThreadStatusToString( int iThreadStatus )
{ switch( iThreadStatus )
   { case CWNET_THREAD_STATUS_NOT_CREATED: return "not created";
     case CWNET_THREAD_STATUS_LAUNCHED   : return "launched";
     case CWNET_THREAD_STATUS_RUNNING    : return "running";
     case CWNET_THREAD_STATUS_TERMINATE  : return "waiting for termination";
     case CWNET_THREAD_STATUS_TERMINATED : return "terminated";
     default : return "invalid thread status";
   }
} // end CwNet_ThreadStatusToString()

//---------------------------------------------------------------------------
char* CwNet_IPv4AddressToString( BYTE *pbIP ) // API (for GUI and 'error log')
{ static char szResult[20];
  sprintf( szResult, "%d.%d.%d.%d",  // oh well, there's some winsock thing for this...
           (int)pbIP[0], (int)pbIP[1],   // ..anyway, use this one,
           (int)pbIP[2], (int)pbIP[3]);  // without so many esoteric tyes
  return szResult;
} // end CwNet_IPv4AddressToString()

//---------------------------------------------------------------------------
void CwNet_PermissionsToString( char **ppszDest, char *pszEndstop, int iPermissions )
{ char *pszStart = *ppszDest;
  char *pszDest;
  if( pszEndstop > pszStart )
   { pszStart[0] = '\0';
     if( iPermissions & CWNET_PERMISSION_TALK )
      { SL_AppendString( ppszDest, pszEndstop, "talk,");
      }
     if( iPermissions & CWNET_PERMISSION_TRANSMIT )
      { SL_AppendString( ppszDest, pszEndstop, "transmit,");
      }
     if( iPermissions & CWNET_PERMISSION_CTRL_RIG )
      { SL_AppendString( ppszDest, pszEndstop, "control the rig,");
      }
     pszDest = *ppszDest;
     if( (pszDest>pszStart) && (pszDest[-1]==',') )
      { pszDest[-1]='.'; // end the list of "permissions" with a dot, not a comma.
      }
     // The resulting string will also appear on the remote client's screen
     // as part of the "Welcome" message, e.g.
     // > Welcome Moritz. You may talk,transmit,control the rig."
     //                           |______"permissions"________|
     // (of course the server's sysop shouldn't use the user names from the manual)
   }
} // end CwNet_PermissionsToString()

//---------------------------------------------------------------------------
char* CwNet_GetCurrentStatusAsString( T_CwNet *pCwNet )   // API (for the GUI)
{
  static char sz127Result[128]; // only called by the GUI, no need to be thread safe,
                                // so a simple static buffer is ok here
  char *cp         = sz127Result;
  char *pszEndstop = sz127Result + 120;

  switch( pCwNet->cfg.iFunctionality )
   { case CWNET_FUNC_CLIENT : // currently acting as a CLIENT ..
        SL_AppendString( &cp, pszEndstop, "Client: " );
        if( pCwNet->iThreadStatus != CWNET_THREAD_STATUS_RUNNING )
         { SL_AppendString( &cp, pszEndstop, CwNet_ThreadStatusToString(pCwNet->iThreadStatus) );
           break;
         }
        else  // the THREAD (in this case, ClientThread() ) is running,
         {    // but that doesn't mean a lot .. the client may not even be
              // connected to the remote server yet. So show what's going on:
           switch( pCwNet->iLocalClientConnState )
            { case CWNET_CONN_STATE_OFF :
                 SL_AppendString( &cp, pszEndstop, "Off (network trouble ?)" );
                 break;
              case CWNET_CONN_STATE_TRY_CONNECT:
                 SL_AppendPrintf( &cp, pszEndstop, "trying to connect server on %s:%d",
                     CwNet_IPv4AddressToString(pCwNet->Client[0].b4HisIP.b),
                                          (int)pCwNet->Client[0].iHisPort );
                 break;
              case CWNET_CONN_STATE_WAIT_RECONN:
                 SL_AppendPrintf( &cp, pszEndstop, "WAITING (%d s) before trying to re-connect",
                    (int)( (CWNET_RECONN_INTERVAL_MS-TIM_ReadStopwatch_ms( &pCwNet->swClientReconnTimer) + 800) / 1000 ) );
                 break;
              case CWNET_CONN_STATE_CONNECTED :
                 // Only just "connected" or have we successfully "logged in" ?
                 switch( pCwNet->Client[0].iClientState ) // only care about Client[0] = the "local client" (not remote)
                  { case CWNET_CLIENT_STATE_LOGIN_CFMD: // "log-in confirmed, identified a 'known user'"
                       SL_AppendPrintf( &cp, pszEndstop, "logged into %s:%d",
                          CwNet_IPv4AddressToString(pCwNet->Client[0].b4HisIP.b),
                                               (int)pCwNet->Client[0].iHisPort );
                       SL_AppendPrintf( &cp, pszEndstop, ", tx=%ld, rx=%d",
                           (long)pCwNet->dwNumBytesSent,
                           (long)pCwNet->dwNumBytesRcvd );
                       if( pCwNet->Client[0].iPermissions != CWNET_PERMISSION_NONE )
                        { SL_AppendString( &cp, pszEndstop, ", may " );
                          CwNet_PermissionsToString( &cp, pszEndstop, pCwNet->Client[0].iPermissions );
                        }
                       break;
                    default:  // CWNET_CLIENT_STATE_CONNECTED .. CWNET_CLIENT_STATE_LOGIN_SENT :
                       SL_AppendPrintf( &cp, pszEndstop, "connected to %s:%d",
                          CwNet_IPv4AddressToString(pCwNet->Client[0].b4HisIP.b),
                                               (int)pCwNet->Client[0].iHisPort );
                       break;
                  } // end switch( pCwNet->Client[0].iClientState )
                 break;
              default :
                 SL_AppendString( &cp, pszEndstop, "Screwed up, see 'Debug' tab." );
                 break;
            } // end switch( pCwNet->iLocalClientConnState )
         } // end else < CWNET_THREAD_STATUS_RUNNING > ?
        break;
     case CWNET_FUNC_SERVER : // currently acting as a SERVER ..
        SL_AppendString( &cp, pszEndstop, "CW-Server: " );
        SL_AppendString( &cp, pszEndstop, CwNet_ThreadStatusToString(pCwNet->iThreadStatus) );
        if( pCwNet->nRemoteClientsConnected > 0 )
         { SL_AppendPrintf( &cp, pszEndstop, ", %d client", (int)pCwNet->nRemoteClientsConnected );
           if( pCwNet->nRemoteClientsConnected > 1 )
            { SL_AppendChar( &cp, pszEndstop, 's' ); // clients, not client :)
            }
         }
        else
         { SL_AppendPrintf( &cp, pszEndstop, ", no client connected" );
         }
        SL_AppendPrintf( &cp, pszEndstop, ", tx=%ld, rx=%d bytes",
                         (long)pCwNet->dwNumBytesSent,
                         (long)pCwNet->dwNumBytesRcvd );
        return sz127Result;
     default: // case CWNET_FUNC_OFF..
        SL_AppendString( &cp, pszEndstop, "off" );
        break;
    } // end switch( pCwNet->cfg.iFunctionality )
  return sz127Result;
} // end CwNet_GetCurrentStatusAsString()

//---------------------------------------------------------------------------
char* CwNet_ClientStateToString( int iClientState )
{ switch( iClientState )
   { case CWNET_CLIENT_STATE_DISCONN : return "disc";
     case CWNET_CLIENT_STATE_ACCEPTED:
     //   CWNET_CLIENT_STATE_CONNECTED: // same value, similar meaning, but for the LOCAL CLIENT side
     case CWNET_CLIENT_STATE_LOGIN_SENT: return "login";
     case CWNET_CLIENT_STATE_LOGIN_CFMD: return "active";
     case CWNET_CLIENT_STATE_LOGIN_HTTP: return "HTTP";
     default : return "???";
   }
} // end CwNet_ClientStateToString()

//---------------------------------------------------------------------------
char* CwNet_GetLastErrorAsString( T_CwNet *pCwNet )      // API (for the GUI)
{ char *cpResult = pCwNet->pszLastError; // sufficiently thread-safe ..
  if( cpResult != NULL )
   { return cpResult;
   }
  else
   { return "no error";
   }
} // end CwNet_GetLastErrorAsString()

//---------------------------------------------------------------------------
int CwNet_GetFreeSpaceInTxBuffer( T_CwNet *pCwNet )
  // Called from various places BEFORE CwNet_AllocBytesInTxBuffer(),
  //     to check in advance if enough bytes can be appended for transmission.
{
  if( pCwNet == NULL ) // oops.. HttpServer.c doesn't have a valid reference anymore ?
   { return 0;  // avoid crashing with a NULL-pointer reference below
     // (the problem was actually fixed in HttpSrv_CreateInstance() ... )
   }
  return CWNET_SOCKET_TX_BUFFER_SIZE - pCwNet->nBytesInTxBuffer;
} // end CwNet_GetFreeSpaceInTxBuffer()


//---------------------------------------------------------------------------
BYTE* CwNet_AllocBytesInTxBuffer( // API - but see restrictions below (*)
                 T_CwNet *pCwNet,
                 BYTE bCommand,   // CWNET_CMD_NONE for HTTP (see HttpServer.c),
                                  // anything else to send our 'binary' protocol
                 int iPayloadSize)
  // Allocates a block of bytes pCwNet->bTxBuffer[0], and fills in the
  // 'Command' and (depending on iPayloadSize) one or two extra header bytes
  // with the BLOCK SIZE. Does NOT fill in the payload itself (but returns
  // a simple byte-pointer to the first PAYLOAD BYTE).
  //   This avoids block-copying by assembling the to-be-transmitted data block
  //   DIRECTLY in the TX buffer (aka 'outbound network buffer') .
  // Returns the address of the first byte in the **PAYLOAD**
  //         when allocation was successful, and if there's a payload to fill at all.
  // If there is no payload to fill, CwNet_AllocBytesInTxBuffer() returns NULL .
  // (*) Because there is no PER-CLIENT transmit buffer, this function
  //     may only be called from the context of the Server- or Client-Thread,
  //     for example from ...
  //       * CwNet_OnReceive() -> CwNet_ExecuteCmd() -> ... -> CwNet_AllocBytesInTxBuffer()
  //       * CwNet_OnReceive() -> HttpSrv_OnReceive() -> .. -> HttpSrv_SendString() -> CwNet_AllocBytesInTxBuffer()
  //       * CwNet_OnPoll() -> CwNet_AllocBytesInTxBuffer(), etc .
{
  BYTE *pb;
  int iOldBytesInTxBuffer = pCwNet->nBytesInTxBuffer;
  int nBytesToAllocate = iPayloadSize;
  (void)nBytesToAllocate;  // "assigned a value that is never used" .. oh, shut up, Mr Pedantic !

  bCommand &= CWNET_CMD_MASK_COMMAND; // strip the "block length indicator bits"
     // (they will be bitwise ORed below, depending on the PAYLOAD SIZE)
  if( bCommand == CWNET_CMD_NONE ) // sending HTTP to a remote client (see HttpServer.c) ?
   { // Do NOT emit a 'binary command byte' before the payload,
     // and also do NOT emit a 'block size indicator' after the command:
     nBytesToAllocate = iPayloadSize;
   }
  else
   { nBytesToAllocate = iPayloadSize + 1; // add one byte for the "command" in our BINARY protocol
     if( iPayloadSize>0 ) // need ONE or TWO extra bytes to indicate the "payload size"..
      {
        if( iPayloadSize <= 255 )  // a ONE-BYTE size indicator will do..
         { nBytesToAllocate += 1;
           bCommand |= CWNET_CMD_MASK_SHORT_BLOCK; // "short block" (ONE-BYTE length, i.e. max 255 byte payload)
         }
        else // payload size doesn't fit in 8 bits -> need a TWO-BYTE size indicator :
         { nBytesToAllocate += 2;  // (will regularly get here with CWNET_CMD_AUDIO=0x11)
           bCommand |= CWNET_CMD_MASK_LONG_BLOCK; // "long block" (TWO-BYTE length, i.e. max 65535 byte payload)
         }
      }
     else  // whow, a single-byte command without any 'payload' at all ->
      { nBytesToAllocate = 1;
      }
   }
  if( nBytesToAllocate <= ( CWNET_SOCKET_TX_BUFFER_SIZE - iOldBytesInTxBuffer ) )
   { // Ok.. enough space left in the "transmit buffer" so ALLOCATE the block,
     //      and already fill out the 1..3 "header"-bytes :
     pCwNet->nBytesInTxBuffer = iOldBytesInTxBuffer + nBytesToAllocate; // NOW the space is occupied ..
     pb = pCwNet->bTxBuffer + iOldBytesInTxBuffer; // new block begins here..
     if( bCommand != CWNET_CMD_NONE )
      { pb[0] = bCommand;  // first header byte (MANDATORY) : 6-bit "command" and 2-bit "size indicator"
        if( (bCommand & CWNET_CMD_MASK_BLOCKLEN) == CWNET_CMD_MASK_SHORT_BLOCK ) // ONE BYTE for the block length ?
         { pb[1] = (BYTE)iPayloadSize;      // fill in the 8-bit blocksize ...
           memset( pb+2, 0, iPayloadSize);  // ... and clean old garbage in the "short payload"
           return pb+2;    // caller shall fill in the payload HERE
         }
        else if( (bCommand & CWNET_CMD_MASK_BLOCKLEN) == CWNET_CMD_MASK_LONG_BLOCK ) // TWO BYTES for the block length ?
         { pb[1] = (BYTE)(iPayloadSize & 0x00FF); // fill in the 16-bit blocksize (little endian, BASTA)
           pb[2] = (BYTE)(iPayloadSize >> 8);     // again, the 6-bit command, 2-bit size code, and up to 2-byte payload-size a NOT counted in 'iPayloadSize' !
           memset( pb+3, 0, iPayloadSize);  // ... and clean old garbage in the "long payload"
           return pb+3;    // caller shall fill in the payload HERE
         }
        else
         { return NULL;    // no PAYLOAD to fill by the caller ("command" already set ?)
         }
      }
     else // bCommand with neither CWNET_CMD_MASK_SHORT_BLOCK nor CWNET_CMD_MASK_LONG_BLOCK ->
      { // The call was made with bCommand = 0 from HttpSrv_SendString() or similar .
        if( iPayloadSize > 0 )
         { memset( pb, 0, iPayloadSize );  // avoid 'leaking out old data'
           return pb;      // caller may now copy e.g. an HTTP RESPONSE here
         }
      }
   } // end if < number of bytes to "allocate" fits inside the "network transmit buffer" >
  return NULL;
} // end CwNet_AllocBytesInTxBuffer()

//---------------------------------------------------------------------------
static BOOL CwNet_SendTextToPrint( T_CwNet *pCwNet, char *pszText )
  // INTERNAL ! Must only be called in the Client- or Server-thread-context !
  // To send text from e.g. the 'GUI thread', use T_CFIFO T_CwNet.ChatTxFifo .
{
  int  iLen = strlen( pszText );
  BYTE *pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_PRINT, iLen+1/*we SEND a trailing zero*/ );
  if( pbPayload != NULL )
   { memcpy( pbPayload, pszText, iLen+1 ); // <- this INCLUDES the string's trailing zero !
     return TRUE;
   }
  return FALSE;  // cannot send now (the network TX buffer is too full, "please try again later")
} // end CwNet_SendTextToPrint()

//---------------------------------------------------------------------------
static BOOL CwNet_SendTxAnnouncement( T_CwNet *pCwNet, char *pszText )
  // Sent from server to all currently connected clients to inform them
  //      who currently "has the key" (or the microphone, maybe..).
  // INTERNAL ! Must only be called from the Server-thread-context !
{
  int  iLen = strlen( pszText );
  BYTE *pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_TX_INFO, 1+iLen+1/*we SEND a trailing zero*/ );
  if( pbPayload != NULL )
   { pbPayload[0] = (BYTE)pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX].iAnnouncedTxClient; // who's transmitting now ?
     memcpy( pbPayload+1, pszText, iLen+1 ); // <- this INCLUDES the string's trailing zero !
     return TRUE;
   }
  return FALSE;  // cannot send now (the network TX buffer is too full, "please try again later")

} // end CwNet_SendTxAnnouncement()

//---------------------------------------------------------------------------
static void CwNet_SendUnifiedParameterReport( T_CwNet *pCwNet,
               int iClientIndex,  // .. for the display in the network traffic log
               T_RigCtrl_ParamInfo *pPI)
{
  BYTE *pbPayload;
  long   i32Value;
  double dblValue;
  switch( pPI->iRigCtrlDataType )
   { case RIGCTRL_DT_INT:  // e.g. for pRC->iOpMode (RIGCTRL_OPMODE_CW/LSB/USB/..)
        i32Value = RigCtrl_GetParamByPN_Int( &pCwNet->RigControl, pPI->iUnifiedPN );
        if( (i32Value != RIGCTRL_NOVALUE_INT) // don't report something we don't know
           &&( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_PARAM_INTEGER,
                              sizeof(T_RigCtrl_ParamReport_Integer) ) ) != NULL )
          )
         { T_RigCtrl_ParamReport_Integer param_report;
           memset( (BYTE*)&param_report, 0, sizeof(param_report) );
           param_report.bRigControlParameterNumber = (BYTE)pPI->iUnifiedPN;
           param_report.i32ParameterValue = i32Value;
           memcpy( pbPayload, (BYTE*)&param_report, sizeof(param_report) );
                 // '--> several dozen milliseconds later, this 'report'
                 //      will arrive at the peer. It the peer is a server,
                 // CwNet_ExecuteCmd() -> RigCtrl_OnParamReport_Integer() -> RigCtrl_SetParamValue_Int()
                 //  will do the rest, and the radio will e.g. switch to the new
                 //  "mode" (modulation type and filter bandwidth),
                 //  e.g. i32ParameterValue = 65 = RIGCTRL_OPMODE_CW (1) | RIGCTRL_OPMODE_VERY_NARROW (64) .
                 //  Only the instance running as a SERVER (directly controlling the radio)
                 //  will "translate" the new 'unified parameter' value
                 //  into e.g. an Icom CI-V command ... not our business here.
           if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // occasionally sent, no "CPU hog"..
            { if( pPI->iUnifiedPN == RIGCTRL_PN_OP_MODE ) // 'operation mode' (USB/LSB/CW..) ? Show as a STRING, not a NUMBER
               { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP,
                    "TX%d IntPara %s = %s", (int)iClientIndex, pPI->pszToken,
                   RigCtrl_OperatingModeToString( (int)param_report.i32ParameterValue ) );
               }
              else // "nothing special" so show the parameter value in numeric form
               { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP,
                    "TX%d IntPara %s = %ld %s", (int)iClientIndex, pPI->pszToken,
                    (long)param_report.i32ParameterValue, pPI->pszUnit );
               }
            } // end if < show the 'integer parameter' report in the traffic log > ?
         }
        break;
     case RIGCTRL_DT_DOUBLE: // e.g. for RIGCTRL_PN_FREQUENCY -> pRC->dblVfoFrequency
        dblValue = RigCtrl_GetParamByPN_Double( &pCwNet->RigControl, pPI->iUnifiedPN );
        if( (dblValue != RIGCTRL_NOVALUE_DOUBLE) // don't report something we don't know
          &&( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_PARAM_DOUBLE,
                              sizeof(T_RigCtrl_ParamReport_Double) ) ) != NULL )
          )
         { T_RigCtrl_ParamReport_Double param_report;
           memset( (BYTE*)&param_report, 0, sizeof(param_report) );
           param_report.bRigControlParameterNumber = (BYTE)pPI->iUnifiedPN;
           param_report.dblParameterValue = dblValue;
           memcpy( pbPayload, (BYTE*)&param_report, sizeof(param_report) );
           if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC )
            { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP,
                 "TX%d DblPara %s = %lf %s", (int)iClientIndex, pPI->pszToken,
                    (double)param_report.dblParameterValue, pPI->pszUnit );
            } // end if < show the 'double parameter' report in the traffic log > ?
         }
        break;

     case RIGCTRL_DT_STRING:        // not supported here yet
        break;
     //case RIGCTRL_DT_FREQ_RANGE:  // not supported here yet
     default: break;
   } // end switch( pPI->iRigCtrlDataType )
} // end CwNet_SendUnifiedParameterReport()

//---------------------------------------------------------------------------
static BOOL CwNet_AddParamToTxQueueForRemoteClient( T_CwNetClient *pClient, int iUnifiedPN )
  // Adds a new item to the queue with 'Unified parameter numbers' that still
  // need to be sent from SERVER to a particular REMOTE CLIENT .
  //
  // [in] iUnifiedPN : e.g. RIGCTRL_PN_OP_MODE, after the operator (GUI user)
  //                   switched between CW, USB, LSB, or switched the bandwidth
  //                   between CW, CWN (narrow), CWNN (very narrow filter).
  // [out] pClient->iTxParameterQueue[ pRC->iTxParameterQueueHeadIndex++ ]
  //       ,---------------------------------------------'
  //       '--> circular FIFO index, runs from 0 to CWNET_TX_PARAMETER_QUEUE_LENGTH-1
  // [in]  pClient->iTxParameterQueueTailIndex
{ int iNewHeadIndex = ( pClient->iTxParameterQueueHeadIndex+1 ) % CWNET_TX_PARAMETER_QUEUE_LENGTH;
  if( iNewHeadIndex == pClient->iTxParameterQueueTailIndex )
   { return FALSE; // error : the queue with to-be-sent parameter numbers is already full !
   }
  // ToDo: Check if the to-be-sent 'unified parameter number' is already in the queue.
  //       If it is, don't append it to the queue.
  pClient->iTxParameterQueue[pClient->iTxParameterQueueHeadIndex] = iUnifiedPN;
  pClient->iTxParameterQueueHeadIndex = iNewHeadIndex;
  return TRUE;
} // end CwNet_AddParamToTxQueueForRemoteClient()

//---------------------------------------------------------------------------
static int CwNet_GetParamNrFromTxQueueForClient( T_CwNetClient *pClient )
  // Returns a "unified parameter number" when there are more parameters
  //         to send from the CwNet-SERVER to a particular remote CLIENT.
  // Return value : RIGCTRL_PN_UNKNOWN (0) when none of the parameters
  //                needs to be sent from server to client,
  //             or RIGCTRL_PN_FREQUENCY, RIGCTRL_PN_OP_MODE, etc etc etc
  //                if one of the 'simple parameters' (with data type int or double)
  //                needs to be sent from THIS SERVER to a remote client.
{ int iUnifiedPN = RIGCTRL_PN_UNKNOWN;
  if( pClient->iTxParameterQueueHeadIndex != pClient->iTxParameterQueueTailIndex ) // circular FIFO not empty ?
   { iUnifiedPN = pClient->iTxParameterQueue[pClient->iTxParameterQueueTailIndex];
     pClient->iTxParameterQueueTailIndex = (pClient->iTxParameterQueueTailIndex+1) % CWNET_TX_PARAMETER_QUEUE_LENGTH;
   }
  return iUnifiedPN;
} // end CwNet_GetParamNrFromTxQueueForClient()

//---------------------------------------------------------------------------
static void CwNet_DuplicateParameterQueueForRemoteClients( T_CwNet *pCwNet )
   // Periodically called from the SERVER THREAD, before entering the loop
   // that 'serves' each remote client .
{ int iUnifiedPN, iClientIndex;
  while( (iUnifiedPN=RigCtrl_GetParamNrFromTxQueueForCwNet(&pCwNet->RigControl)) > RIGCTRL_PN_UNKNOWN ) // Here: SERVER side, with an extra queue for each REMOTE CLIENT...
   { for( iClientIndex=1/*!*/; iClientIndex<=/*!*/CWNET_MAX_CLIENTS; ++iClientIndex )
      { CwNet_AddParamToTxQueueForRemoteClient( &pCwNet->Client[iClientIndex], iUnifiedPN );
      }
   }
} // end CwNet_DuplicateParameterQueueForRemoteClients()


//---------------------------------------------------------------------------
// Microscopic subset of 'Rigctrd' / 'Hamblib' compatible commands
//   supported "internally" by CwNet (embedded in the same TCP/IP stream
//   that also transports audio, keying signals, spectrum waveforms etc,
//   thus never compatible with the original 'rigctld' protocol) :
//---------------------------------------------------------------------------


// Internal forward declaractions for the functions in CwNet_Rigctld_Commands[] .
// SEQUENCE as listed at hamlib.sourceforge.net/html/rigctld.1.html .
static int CwNet_Rigctld_OnGetFreq(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnSetFreq(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnGetMode(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnSetMode(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnGetVFO( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnSetVFO( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnGetRIT( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnSetRIT( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnGetXIT( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnSetXIT( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnGetPTT( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnSetPTT( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs, char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnGetSplitVFO(T_CwNet *pCwNet, T_CwNetClient *pClient,T_CwNet_CmdTabEntry *pCmdInfo,const char *pszCmdArgs,char *pszResp,int iMaxRespLength );
static int CwNet_Rigctld_OnSetSplitVFO(T_CwNet *pCwNet, T_CwNetClient *pClient,T_CwNet_CmdTabEntry *pCmdInfo,const char *pszCmdArgs,char *pszResp,int iMaxRespLength );
static int CwNet_Rigctld_OnDumpState(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs,char *pszResp,int iMaxRespLength );
static int CwNet_Rigctld_OnCheckVFO(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs,char *pszResp, int iMaxRespLength );
static int CwNet_Rigctld_OnQuit( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo, const char *pszCmdArgs,   char *pszResp, int iMaxRespLength );


// Table with all tokens (long and short) and their handlers (C functions) :
//   [Without modifications, Mr. Pedantic BCB V12 complained:
//     > [bcc32c Warning] : incompatible pointer types initializing
//     > 'CwNetCommandHandler'
//     > (aka 'int (*)(struct t_HLSrv *, struct t_HLSrvClient *,
//     >        struct t_CwNet_CommandTableEntry *, const char *)')
//     > with an expression of type 'int (T_HLSrv *, T_CwNetClient *, -,
//     >        T_CwNet_CmdTabEntry *, const char *)'                  |
//     > (aka 'int (struct t_HLSrv *, struct t_HLSrvClient *,          |
//     >        struct t_CwNet_CommandTableEntry *, const char *)')    |
//     Fixed by inserting the SUPERFLUENT CASTS in the table below.]   |
static const T_CwNet_CmdTabEntry CwNet_Rigctld_Commands[] =  //        |
{                                                    //  ,-------------'
  // ShortToken LongToken Access             Handler     |
  { 'f',     "get_freq",  HLSRV_ACCESS_GET,  (CwNetCommandHandler)CwNet_Rigctld_OnGetFreq },
  { 'F',     "set_freq",  HLSRV_ACCESS_SET,  (CwNetCommandHandler)CwNet_Rigctld_OnSetFreq },

  { 'm',     "get_mode",  HLSRV_ACCESS_GET,  (CwNetCommandHandler)CwNet_Rigctld_OnGetMode },
  { 'M',     "set_mode",  HLSRV_ACCESS_SET,  (CwNetCommandHandler)CwNet_Rigctld_OnSetMode },

  { 'q',     "exit rigctl",HLSRV_ACCESS_GET, (CwNetCommandHandler)CwNet_Rigctld_OnQuit },
  { 'Q',     "exit_rigctl",HLSRV_ACCESS_GET, (CwNetCommandHandler)CwNet_Rigctld_OnQuit },

  { 's',  "get_split_vfo",HLSRV_ACCESS_GET,  (CwNetCommandHandler)CwNet_Rigctld_OnGetSplitVFO },
  { 'S',  "set_split_vfo",HLSRV_ACCESS_SET,  (CwNetCommandHandler)CwNet_Rigctld_OnSetSplitVFO },

  { 't',     "get_ptt",   HLSRV_ACCESS_SET,  (CwNetCommandHandler)CwNet_Rigctld_OnGetPTT }, // from WSJT-X: "t VFOA\n"
  { 'T',     "set_ptt",   HLSRV_ACCESS_SET,  (CwNetCommandHandler)CwNet_Rigctld_OnSetPTT }, // from WSJT-X: "T VFOA 0\n" (0=RX, 1=TX, 2=TX_mic, 3=TX_data)

  { 'v',     "get_vfo",   HLSRV_ACCESS_GET,  (CwNetCommandHandler)CwNet_Rigctld_OnGetVFO },
  { 'V',     "set_vfo",   HLSRV_ACCESS_SET,  (CwNetCommandHandler)CwNet_Rigctld_OnSetVFO },

  { 0x00,    "dump_state",HLSRV_ACCESS_GET,  (CwNetCommandHandler)CwNet_Rigctld_OnDumpState},

  { 0x00,    "chk_vfo",   HLSRV_ACCESS_GET,  (CwNetCommandHandler)CwNet_Rigctld_OnCheckVFO },


  // End of the table marked by 'all zeros' (and NULL pointers):
  { 0x00,    NULL,        0,                 NULL }


  // Note: Similar incarnations of this 'hamlib server command table' may exist
  //          in Remote_CW_Keyer/HamlibServer.c   and   Remote_CW_Keyer/CwNet.c !
  //               ,--------------------'               ,---------------'
  //              "full-blown" but without support     "stripped down subset"
  //              for AUDIO, KEYING, and SPECTRA       of the original 'hamlib'
  //              tunneled over the same TCP/IP        command set .
  //              socket !

}; // end CwNet_Rigctld_Commands[]


//---------------------------------------------------------------------------
static BOOL CwNet_SendRigctldCompatibleString( T_CwNet *pCwNet,
              T_CwNetClient *pClient,
              const char *pszText )
  // Sends from client to server, or (maybe) also from server to client,
  //     with a 'rigctld' compatible ASCII string
  //     (btw the 'd' means 'daemon' or something like that. We keep it to emphasize
  //      that this is not the Remote CW Keyer's own "RigControl" but "rigctld".)
  // Returns TRUE if the string could be placed in the transmit buffer.
  //     In that case, the caller (e.g. CwNet_OnPoll()) can copy the
  //     'current' parameter value into 'last sent value' for an event-driven
  //     transmission ("only sent when modified" to keep the bandwidth low).
  //
  // INTERNAL function ! Must only be called from the Server-thread-context !
  // Callers :
  //   * CwNet_Rigctld_PrintToResponse()
  //   * CwNet_OnPoll() [CLIENT SIDE],  to send e.g. "set_ptt", etc, to the server
{
  int iClientIndex = (int)(pClient - &pCwNet->Client[0]); // perfectly legal "pointer trick"..
  int  iLen = strlen( pszText );
  char sz80HexDump[84];
  BYTE *pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_RIGCTLD, iLen+1 );
           //   ,-----------------------------------------------------------'
           //   '--> The trailing zero is included in the transmitted 'payload'
           //        to simplify parsing it as a zero-terminated C string,
           //        especially if one fine day the SERVER runs on a simple
           //        low-power microcontroller (e.g. PIC or dsPIC) .
  if( pbPayload != NULL )
   { memcpy( pbPayload, pszText, iLen+1 ); // <- this INCLUDES the string's trailing zero !
     if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
      { // don't assume 'pszText' is always a plain ASCII string w/o control characters !
        // We don't want e.g. those '\n' characters in the output, so:
        INET_DumpTraffic_HexOrASCII( sz80HexDump, 80, (BYTE*)pszText, iLen );
        ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d Rigctld %s",(int)iClientIndex, sz80HexDump );
      }
     return TRUE;
   }
  return FALSE;  // cannot send now (the network TX buffer is too full, "please try again later")
} // end CwNet_SendRigctldCompatibleString()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_PrintToResponse( T_CwNet *pCwNet,
              T_CwNetClient *pClient, char *pszDest, int iMaxDestLength,
              const char *pszFormat, ... )
  // "Prints" into a string buffer for the response (which is usually
  //          the network transmit buffer) :
  //           * the "printed text" must be short enough to fit inside
  //             the network transmit buffer - see CWNET_SOCKET_TX_BUFFER_SIZE .
  //           * with a power-saving microcontroller firmware in mind,
  //             the whole thing does NOT use dynamic memory allocation at all !
  //           * even if CWNET_SOCKET_TX_BUFFER_SIZE may be as large as 16 kByte
  //             for the GUI variant, keep the strings MUCH shorter,
  //             and forget about scary monsters like hamlib's "dump_state" .
  // Return value: Number of characters actually "printed" into pszResp .
{
  va_list parameter;   // argument list for VA_... macros
  int nCharsPrinted;
  va_start( parameter, pszFormat );
  // Can we safely assume vsnprintf() will ALWAYS appended a trailing zero ? No, we can't ! Thus:
  memset( pszDest, 0, iMaxDestLength );
  vsnprintf( pszDest, iMaxDestLength - 1/*!*/, pszFormat, parameter );
  va_end( parameter );  // WE called va_start(), so WE call va_end() [because vsnprintf() does NOT do that]
  if( nCharsPrinted < 0 ) // oops... something went REALLY wrong !
   { return 0;
   }
  else if( nCharsPrinted < iMaxDestLength )
   { // ok, the caller's string buffer was long enough .. nothing truncated
     return nCharsPrinted; // don't waste time with strlen()
   }
  else
   { return strlen( pszDest );
   }
} // end CwNet_Rigctld_PrintToResponse()


//---------------------------------------------------------------------------
void CwServer_FeedActivityWatchdog( T_CwNetClient *pClient )
{
  TIM_StartStopwatch( &pClient->swActivityWatchdog ); // feed the dog,
  // so it won't bark or bite for the next <CWNET_ACTIVITY_TIMEOUT_MS> milliseconds.
} // end CwServer_FeedActivityWatchdog()

//---------------------------------------------------------------------------
static void CwServer_OnConnect( T_CwNet *pCwNet, T_CwNetClient *pClient )
  // Called from ServerThread() after a new REMOTE CLIENT has joined in.
  // We didn't receive anything from the guy on the other side ("peer") yet,
  // but we may already return a few bytes as the 'greeting message' from here.
  //    This can even be tested with a WEB BROWSER instead of a remote client,
  //    by entering e.g. "http://MyServer.dynv6.net:7355/" as URL on a smartphone)
  // [out, optional] pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ]
  //     = "anything to send", limited to
  //       CWNET_SOCKET_TX_BUFFER_SIZE - pCwNet->nBytesInTxBuffer (on entry) .
{
  int iClientIndex = (int)(pClient - &pCwNet->Client[0]); // perfectly legal "pointer trick":
      // '-->  for the server's list of "remote clients",
      //       iClientIndex starts at ONE because Client[0]
      //       is the "local client" (used by ClientThread(), not ServerThread() )

  CwServer_FeedActivityWatchdog( pClient ); // feed the dog, so it won't bite ...
                       // for the next <CWNET_ACTIVITY_TIMEOUT_MS> milliseconds.
  pClient->dblUnixTimeOfStart = UTL_GetCurrentUnixDateAndTime(); // <- here: set immediately in CwServer_OnConnect()
  TIM_StartStopwatch( &pClient->sw_VfoReport ); // send a VFO REPORT on the next occasion, and ....
  // Send the following parameters (by 'Unified Parameter Numbers') to this client on the next occasion:
  CwNet_AddParamToTxQueueForRemoteClient( pClient, RIGCTRL_PN_OP_MODE );
  CwNet_AddParamToTxQueueForRemoteClient( pClient, RIGCTRL_PN_SCOPE_MODE );
  CwNet_AddParamToTxQueueForRemoteClient( pClient, RIGCTRL_PN_SCOPE_REF_LEVEL );
  CwNet_AddParamToTxQueueForRemoteClient( pClient, RIGCTRL_PN_SCOPE_SPAN );
        // Note: A few (spectrum-)scope related parameters are missing here,
        //       because those parameters are sent as part of a T_CwNet_SpectrumHeader,
        //       and without spectrum data, the remote client doesn't need them anyway.
  CwNet_AddParamToTxQueueForRemoteClient( pClient, RIGCTRL_PN_SCOPE_EDGE_NR );
  CwNet_AddParamToTxQueueForRemoteClient( pClient, RIGCTRL_PN_SCOPE_SPEED );
        // Not read from the radio yet: CwNet_AddParamToTxQueueForRemoteClient( pClient, RIGCTRL_PN_SCOPE_VBW  );


  if( pCwNet->cfg.iDiagnosticFlags & (CWNET_DIAG_FLAGS_VERBOSE | CWNET_DIAG_FLAGS_SHOW_CONN_LOG) )
   {
     ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP,
         "Server: Accepted connection from client #%d on %s:%d (no data yet)",
         (int)(iClientIndex+1),
         CwNet_IPv4AddressToString( pClient->b4HisIP.b ), (int)pClient->iHisPort );
     // Added HERE (in OnConnect()) because for strange reasons,
     // a certain web browser connected our server ONE MULTIPLE SOCKETS,
     // but then decided to use only ONE of THREE for actual data :
     // > 14:33:36.2 Server: Accepted connection from 127.0.0.1:55031 (no data yet)
     // > 14:33:36.2 Server: Accepted connection from 127.0.0.1:55029 (no data yet)
     // > 14:33:36.9 RX1 [0191] GET /LiveData.htm HTTP/1.1\r\nHost: ...
     // > 14:33:36.9 TX1 [0127] HTTP/1.1 200 OK\r\nContent-Type:text/html; ...
     // > 14:33:37.9 RX1 [0191] GET /LiveData.htm HTTP/1.1\r\nHost: ...
     // > 14:33:37.9 TX1 [0127] HTTP/1.1 200 OK\r\nContent-Type:text/html; ...
     // > 14:33:39.0 RX1 [0191] GET /LiveData.htm HTTP/1.1\r\nHost: ...
     // > 14:33:39.0 TX1 [0127] HTTP/1.1 200 OK\r\nContent-Type:text/html; ...
     // > 14:33:40.0 RX1 [0191] GET /LiveData.htm HTTP/1.1\r\n      ...
     // > 14:33:40.0 TX1 [0127] HTTP/1.1 200 OK\r\nContent-Type:    ...
     // > 14:33:41.0 RX1 [0191] GET /LiveData.htm HTTP/1.1\r\nHost: ...
     // > 14:33:41.0 TX1 [0127] HTTP/1.1 200 OK\r\nContent-Type:    ...
     // > 14:33:41.2 Server: Disconnected 127.0.0.1:55031 (timeout)
     // > 14:33:41.2 Server: Disconnected 127.0.0.1:55029 (timeout)

   } // end if < show 'accepted connections'> ?

  // On the 'CW Server' side, don't send anything immediately .
  // Let the PEER (remote client) send his first few bytes (and check what
  // 'he' is) . As long we we haven't verified he's a REMOTE CW CLIENT,
  // he won't receive a single byte from this server. See implementation
  // of CwNet_OnPoll( ) .

# if( SWI_USE_VORBIS_STREAM ) // allow streaming audio via Ogg/Vorbis ?
  pClient->dwNextVorbisPage = pCwNet->sVorbis.dwPageIndex+1; // avoid starting to play "in the past"
# endif // SWI_USE_VORBIS_STREAM

} // end CwServer_OnConnect()

//---------------------------------------------------------------------------
static void CwServer_OnDisconnect( T_CwNet *pCwNet, T_CwNetClient *pClient )
  // Called from ServerThread() after a REMOTE CLIENT disconnected
  //                            (thus it's too late to say 'goodbye' here).
  // Note: Besides 'graceful' TCP disconnect, there's also the
  //               'unexpect connection reset'. To keep things simple,
  //  we don't care WHY and HOW a connection was "cut"/"closed"/"reset".
{
  int iClientIndex = (int)(pClient - &pCwNet->Client[0]); // perfectly legal "pointer trick":

  if( (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
    &&( ! pClient->fDisconnect ) // only show this if NOT DISCONNECTED YET :
    )
   { // Will typically get here when CLOSING THE REMOTE WEB BROWSER ...
     ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Server: Disconnected by client #%d on %s:%d",
                (int)(iClientIndex+1), CwNet_IPv4AddressToString(pClient->b4HisIP.b),
                (int)pClient->iHisPort ); // (*)
     if( (pClient->dwNumBytesRcvd + pClient->dwNumBytesSent) > 0 )
      { ShowError( ERROR_CLASS_INFO, "        %ld bytes rvcd, %ld sent.",
         (long)pClient->dwNumBytesRcvd, (long)pClient->dwNumBytesSent );
      }
   } // end if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )

} // end CwServer_OnDisconnect()

//---------------------------------------------------------------------------
int  CwNet_CheckUserAndGetPermissions( // API (also called from HttpServer.c)
        T_CwNet *pCwNet,     // [in] pCwNet->cfg.sz255AcceptUsers
        char *pszUserName,   // [in] e.g. "Moritz" (received from the 'log-in request',
                             //      or fron a QUERY STRING if HTTP is in use.
        char *pszCallsign)   // [in] callsign (which MAY OPTIONALLY be tied to a user name)
{ int n, iPermissions;
  const char *cpCharAfterDelimiter;
  const char *cpSrc = pCwNet->cfg.sz255AcceptUsers;
  char c;
  while( *cpSrc != '\0' )
   {
     cpCharAfterDelimiter = cpSrc; // look ahead for the next colon, or other deliminters after the USER NAME in pCwNet->cfg.sz255AcceptUsers :
     n = SL_SkipCharsUntilDelimiter( &cpCharAfterDelimiter, ":, "/*delimiters*/, SL_SKIP_NORMAL );
       // '--> "Returns the NUMBER OF CHARACTERS actually skipped, INCLUDING the delimiter."
     if( n > 0 )
      {
        if( cpCharAfterDelimiter[-1] == ':' )    // in sz255AcceptUsers: e.g. "Moritz:7" if this user may talk to others, transmit, and even control the radio
         { n = cpCharAfterDelimiter - cpSrc - 1; // n = number of characters that MUST MATCH (below)
           iPermissions = SL_atoi( cpCharAfterDelimiter );
         }
      }
     if( (n>0) && (SL_strncmp( pszUserName, cpSrc, n) == 0) )
      { c = cpSrc[n]; // valid "delimiter" in the list ?
        if( (c==',') || (c==' ') || (c==':') || (c=='\0') )
         { return iPermissions;
         }
      }
     if( SL_SkipCharsUntilDelimiter( // ... "up to, and INCLUDING, the delimiter-character"
            &cpSrc, ", "/*delimiters*/, SL_SKIP_NORMAL ) <= 0 )
      { // nothing more to skip, not even a delimiter (here: comma or space) ->
        break; // end of the comma- or space delimited list, no matching user name
      }
   }
  return -1;   // unrecognized user name -> no permissions at all (not even "zero")
} // end CwNet_CheckUserNameAndGetPermissions()

//---------------------------------------------------------------------------
int CwNet_GetUserPermissionsByRecentIP( T_CwNet *pCwNet, BYTE *pbIPv4Address )
{ int i, t_ms;
  T_CwNetRecentClientEntry *pRecentClient;
  DWORD dwIP;
  memcpy( &dwIP, pbIPv4Address, 4 ); // who knows if pbIPv4Address is DWORD-aligned ?

  for( i=0; i<CWNET_MAX_RECENT_CLIENTS; ++i)
   { pRecentClient = &pCwNet->sRecentClients[i];
     t_ms = TIM_ReadStopwatch_ms( &pRecentClient->swLastSeen );
     if(  (dwIP == pRecentClient->b4HisIP.dw ) &&( t_ms < 10000 )
       )
      { return pRecentClient->iPermissions;
      }
   }
  return -1;  // no idea who this guy is; don't let him in
              // without at least a known USER NAME !
} // end CwNet_GetUserPermissionsByRecentIP()

//---------------------------------------------------------------------------
void CwNet_RefreshUserPermissionsByIP( T_CwNet *pCwNet, BYTE *pbIPv4Address,
                                       int iPermissions )
{ int i, t_ms;
  T_CwNetRecentClientEntry *pRecentClient;
  DWORD dwIP;
  memcpy( &dwIP, pbIPv4Address, 4 ); // who knows if pbIPv4Address is DWORD-aligned ?

  for( i=0; i<CWNET_MAX_RECENT_CLIENTS; ++i)
   { pRecentClient = &pCwNet->sRecentClients[i];
     if( dwIP == pRecentClient->b4HisIP.dw )
      { TIM_StartStopwatch( &pRecentClient->swLastSeen );
        pRecentClient->iPermissions = iPermissions;
        return;
      }
   }
  // Arrived here: The specified IP address isn't in our list anymore,
  // so recycle an outdated entry (or one that hasn't been used before):
  for( i=0; i<CWNET_MAX_RECENT_CLIENTS; ++i)
   { pRecentClient = &pCwNet->sRecentClients[i];
     t_ms = TIM_ReadStopwatch_ms( &pRecentClient->swLastSeen );
     if( (pRecentClient->b4HisIP.dw == 0 ) // hasn't been in use before, or ..
       ||(t_ms > 30000 ) // expired long ago (older than 30 seconds)
       )
      { pRecentClient->b4HisIP.dw = dwIP;
        TIM_StartStopwatch( &pRecentClient->swLastSeen );
        pRecentClient->iPermissions = iPermissions;
        return;
      }
   }
} // end CwNet_RefreshUserPermissionsByIP()

//---------------------------------------------------------------------------
static void CwNet_ExecuteCmd( T_CwNet *pCwNet,
              T_CwNetClient *pClient ) // [in] instance data for a CLIENT
  // Called from CwNet_OnReceive() when a received command is "ready for processing",
  //        including all bytes belonging to the command's data block.
  //        (for special commands like CWNET_CMD_MORSE and CWNET_CMD_AUDIO,
  //         the received DATA BYTES have already been passed to their
  //         dedicated receive-FIFOs, to avoid having to wait for the
  //         reception of the TCP segment with the END of the command's data).
  // [in]  pClient->iCurrentCommand (with the length-indicator-bits already stripped),
  //       pClient->iCmdDataLength,
  //       pClient->bCmdDataBuffer[0..iCmdDataLength-1]
  //         (for 'sufficiently short blocks', already terminated like a C-string)
  // [out, optional] pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ]
  //      = "anything to send", limited to
  //         CWNET_SOCKET_TX_BUFFER_SIZE - pCwNet->nBytesInTxBuffer (on entry)
{
  int  iClientIndex = (int)(pClient - &pCwNet->Client[0]); // again the "pointer trick" to get an array index (in C)
  int  i,n;
  int  nOldBytesInTxBuffer = pCwNet->nBytesInTxBuffer;
  int  nBytesAppended;
  int  iPingLatency_ms;
  BYTE *pbPayload;
  BOOL fOk = FALSE;
  char sz255[256], *cpDest, *cpEndstop;
  long i32;

  switch( pClient->iCurrentCommand )
   { case CWNET_CMD_NONE   : // dummy command to send HTTP instead of our binary protocol ?
        break;
     case CWNET_CMD_CONNECT: // "Connect" (first command, actually a struct, sent from client to server),
        // immediately followed by two strings from CWNET_CMD_PRINT in pClient->bCmdDataBuffer (user name + callsign).
        if( pClient->iCmdDataLength == sizeof(T_CwNet_ConnectData) ) // ok .. length as expected
         { T_CwNet_ConnectData connData;
           // memcopy the received block into a T_CwNet_ConnectData as a local variable,
           // because the source (pClient->bCmdDataBuffer[0..pClient->iCmdDataLength-1])
           // may not be properly aligned to safely access 32- or 64-bit-members
           // on certain processors or controllers (e.g. ARM) :
           memcpy( &connData, pClient->bCmdDataBuffer, sizeof(T_CwNet_ConnectData) );
           //
           if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
            { INET_DumpTraffic_HexOrASCII( sz255, 100, pClient->bCmdDataBuffer, pClient->iCmdDataLength );
              ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d Conn %s", (int)iClientIndex, sz255 );
            }
           if( iClientIndex != CWNET_LOCAL_CLIENT_INDEX ) // here the specific part for the SERVER side:
            { int iPermissions = CwNet_CheckUserAndGetPermissions( pCwNet, connData.sz40UserName, connData.sz40Callsign );
              if( iPermissions >= 0 ) // bingo.. "let this KNOWN user in" and say WELCOME :
               {  pClient->iClientState = CWNET_CLIENT_STATE_LOGIN_CFMD;
                  pClient->iPermissions = iPermissions;
                  SL_strncpy( pClient->sz80UserName, connData.sz40UserName, 40 );
                  SL_strncpy( pClient->sz80Callsign, connData.sz40Callsign, 40 );
                  if( pClient->sz80Callsign[0] <= ' ' ) // without a callsign, the client cannot transmit !
                   { pClient->iPermissions &= ~CWNET_PERMISSION_TRANSMIT;
                   }
                  connData.dwPermissions = (DWORD)pClient->iPermissions;
                  // Send back the "filled out connection data", including the client's permissions:
                  if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_CONNECT, sizeof(T_CwNet_ConnectData)/*iPayloadSize*/) ) != NULL )
                   { memcpy( pbPayload, &connData, sizeof(T_CwNet_ConnectData) );
                   }
                  // Append ANOTHER block to the transmit buffer, with the human-readable 'Welcome'..
                  cpDest = sz255;
                  cpEndstop = sz255+200;
                  SL_AppendPrintf( &cpDest, cpEndstop, "Welcome %s.", pClient->sz80UserName );
                  if( pClient->iPermissions != CWNET_PERMISSION_NONE )
                   { SL_AppendString( &cpDest, cpEndstop, " You may ");
                     CwNet_PermissionsToString( &cpDest, cpEndstop, pClient->iPermissions );
                   }
                  CwNet_SendTextToPrint( pCwNet, sz255 ); // -> CWNET_CMD_PRINT (also appended to the network tx buffer)
                  nBytesAppended = pCwNet->nBytesInTxBuffer - nOldBytesInTxBuffer;
                  // '--> For a possible implementation on a uC :
                  //      nBytesAppended ( = sizeof(T_CwNet_ConnectData) + length of the "welcome" message)
                  //      was approximately 150 .  Even an old PIC can do this.
                  (void)nBytesAppended; // "never used".. oh shut up, we want to examine this in the debugger !
               }
              else // unknown user or a typo -> politely say farewell (because the visitor seems to run the 'Remote CW Keyer' software)
               { pClient->fDisconnect = TRUE;
               }
            } // end if < "server side" >
           else // here the implementation for the CLIENT side (CWNET_CMD_CONNECT as a RESPONSE, with some info from server to client):
            { pClient->iPermissions = (int)connData.dwPermissions;
               // ,-----'
               // '--> The client stores the permissions assigned to him
               //      from the server. If the client misbehaves later
               //      and tries something that the server didn't permit,
               //      he may be kicked out or blacklisted temporarily.
            } // end else < CWNET_CMD_CONNECT received on the client- or server side > ?
         } // end if < received CWNET_CMD_CONNECT with the expected payload length > ?
        break; // end case pClient->iCurrentCommand == CWNET_CMD_CONNECT
     case CWNET_CMD_DISCONN: // "Disconnect" (at any time, in any direction)
        if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
         { INET_DumpTraffic_HexOrASCII( sz255, 100, pClient->bCmdDataBuffer, pClient->iCmdDataLength );
           ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d Disc", (int)iClientIndex );
         }
        break; // end case pClient->iCurrentCommand == CWNET_CMD_DISCONN
     case CWNET_CMD_PING: // send or evaluate the response for a "Ping Test" ?
        if( pClient->iCmdDataLength == CWNET_PAYLOAD_SIZE_PING ) // ok .. length as expected
         { // Unlike the ICMP-ping, our "TCP ping" does not only check the connection,
           // but also tries to measure the network latencies as follows:
           //   REQUEST (e.g. from server to a client)
           //         payload[0] = 0           ("request")
           //         payload[1] = identifier  (for simplicity, the server's "client index")
           //         payload[ 2.. 3] = "future reserve"
           //         payload[ 4.. 7] = i32PingTimestamp_ms[0] = the requester's initial reading of TIM_ReadHighResTimer_ms()
           //         payload[ 8..11] = i32PingTimestamp_ms[1] = still zero (the reciplient fills this when assembling his FIRST response)
           //         payload[12..15] = i32PingTimestamp_ms[2] = still zero (the reciplient of the FIRST response fills this when assembling a SECOND response)
           //   FIRST RESPONSE (e.g. from client to server)
           //         payload[0] = 1           ("first response")
           //         payload[1] = the same identifier as in the original REQUEST
           //         payload[ 2.. 3] = "future reserve"
           //         payload[ 4.. 7] = i32PingTimestamp_ms[0] = still the requester's result from TIM_ReadHighResTimer_ms()
           //         payload[ 8..11] = i32PingTimestamp_ms[1] = now the responder's own reading from TIM_ReadHighResTimer_ms()
           //         payload[12..15] = i32PingTimestamp_ms[2] = still zero
           //   SECOND RESPONSE (back from THE ORIGINAL REQUESTER, e.g. from server to client) !
           //         payload[0] = 2           ("second response")
           //         payload[1] = the same identifier as in the original REQUEST
           //         payload[ 2.. 3] = "future reserve"
           //         payload[ 4.. 7] = i32PingTimestamp_ms[0] = still the requester's result from TIM_ReadHighResTimer_ms()
           //         payload[ 8..11] = i32PingTimestamp_ms[1] = still the responder's own reading from TIM_ReadHighResTimer_ms()
           //         payload[12..15] = i32PingTimestamp_ms[2] = now the original requester's SECOND result from TIM_ReadHighResTimer_ms()
           //  ,--------'
           //  '--> This allows not only the ORIGINAL REQUESTER (e.g. server),
           //       but also the RESPONDER (e.g. client) to know the network latency.
           //       Both ends (peers) then know how much audio- and Morse code samples
           //       must be kept in the buffer, to avoid output buffer underruns, etc.
           if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
            &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
                 ||(pClient->nPeriodicMessagesInTrafficLog < 10 ) )                      // .. except for the first few messages
             )
            { ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d Ping[%d] t1=%ld ms, t2=%ld ms, t3=%ld ms",
                         (int)iClientIndex, (int)pClient->bCmdDataBuffer[0],
                         (long)CwNet_ReadNonAlignedInt32( pClient->bCmdDataBuffer+4),
                         (long)CwNet_ReadNonAlignedInt32( pClient->bCmdDataBuffer+8),
                         (long)CwNet_ReadNonAlignedInt32( pClient->bCmdDataBuffer+12) );
              if(  pClient->nPeriodicMessagesInTrafficLog < 32767 )
               { ++pClient->nPeriodicMessagesInTrafficLog;
               }
            }
           switch( pClient->bCmdDataBuffer[0] ) // ping-REQUEST (0) or -RESPONSE (1,2) ?
            { case 0 : // the peer (so far, always the SERVER) sent a ping-REQUEST,
                       // so on this side (the CLIENT) answer with a REPONSE:
                 if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_PING, CWNET_PAYLOAD_SIZE_PING) ) != NULL )
                  {
                    // Store the FIRST 32-bit timestamp for later (the "original requester's" high-res-timestamp):
                    memcpy( &pClient->i32PingTimestamp_ms[0], pClient->bCmdDataBuffer+4, 4);

                    // Start a local stopwatch (with higher resolution than the MILLISECONDS sent over the network):
                    TIM_StartStopwatch( &pClient->swPing );   // <- client side, started on reception of a ping-REQUEST

                    // Assemble the FIRST RESPONSE (details above) :
                    pbPayload[0] = 1;                          // "1st response"
                    pbPayload[1] = pClient->bCmdDataBuffer[1]; // "ping-ID"
                    pbPayload[2] = pClient->bCmdDataBuffer[2]; // future reserved / alignment dummy ..
                    pbPayload[3] = pClient->bCmdDataBuffer[3];
                    if( iClientIndex == CWNET_LOCAL_CLIENT_INDEX ) // If this instance runs as a CLIENT,
                     { // synchronize our the "high-resolution timer" to the REMOTE CLIENT
                       // (of course, only inside this program. See implementation of Timers.c)
                       TIM_SyncHighResTimer_ms( pClient->i32PingTimestamp_ms[0] );
                     }
                    pClient->i32PingTimestamp_ms[1] = TIM_ReadHighResTimer_ms() & 0x7FFFFFFF;
                    // copy the THREE TIMESTAMPS into the new payload (again, KISS, little endian byte order):
                    memcpy( pbPayload+4, pClient->i32PingTimestamp_ms, 3*4);
                    //
                    if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                     &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
                          ||(pClient->nPeriodicMessagesInTrafficLog < 10 ) )                      // .. except for the first few messages
                      )
                     { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d Ping[%d] t1=%ld ms, t2=%ld ms, t3=%ld ms",
                         (int)iClientIndex, (int)pbPayload[0],
                         (long)CwNet_ReadNonAlignedInt32( pbPayload+4 ),
                         (long)CwNet_ReadNonAlignedInt32( pbPayload+8 ),
                         (long)CwNet_ReadNonAlignedInt32( pbPayload+12) );
                       if(  pClient->nPeriodicMessagesInTrafficLog < 32767 )
                        { ++pClient->nPeriodicMessagesInTrafficLog;
                        }
                     }
                  } // end < successfully appended the FIRST RESPONSE in the network buffer > ?
                 break;
              case 1 : // the peer (usually A CLIENT) sent the first ping-RESPONSE, so evaluate the result,
                       // and send back a SECOND response so BOTH peers know the latencies .
                 // Note: 'case 1' with "client #1..N" will be seen on the SERVER SIDE.
                 //       'case 2' with "client #0" will be seen on the CLIENT SIDE.
                 //
                 // Take a snapshot for the THIRD timestamp in the SECOND response (see principle above):
                 pClient->i32PingTimestamp_ms[2] = TIM_ReadHighResTimer_ms() & 0x7FFFFFFF;
                 if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_PING, CWNET_PAYLOAD_SIZE_PING) ) != NULL )
                  {
                    // Store the SECOND 32-bit timestamp for later (the "first responder's" high-res-timestamp):
                    memcpy( &pClient->i32PingTimestamp_ms[1], pClient->bCmdDataBuffer+8, 4);

                    if( (pClient->bCmdDataBuffer[1] == (BYTE)iClientIndex ) // "ID" as expected, or ..
                      ||(iClientIndex == CWNET_LOCAL_CLIENT_INDEX ) ) // ..this is the CLIENT side ?
                     { i32 = TIM_ReadStopwatch_us( &pClient->swPing );
                       if( (i32 >= 0) && (i32 < 2000000) ) // "realistic" (time here still in MICROseconds) ?
                        { // ex: iPingLatency_ms = (i32 + 999) / 1000;
                          iPingLatency_ms = pClient->i32PingTimestamp_ms[2]-pClient->i32PingTimestamp_ms[0];
                          // '--> this is the same difference (in ms) as calculated
                          //      on "the other side" (see 'case 2' further below)
                          if( iPingLatency_ms >= pClient->iPingLatency_ms ) // latency now HIGHER ->
                           { // let pClient->iPingLatency_ms jump to the new peak
                             pClient->iPingLatency_ms = iPingLatency_ms;
                           }
                          else // ping latency now lower -> let pClient->iPingLatency_ms DROP SLOWLY
                           { pClient->iPingLatency_ms -= (pClient->iPingLatency_ms-iPingLatency_ms) / 10;
                           }
                          if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
                           { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Ping test, client #%d: %d ms, pk = %d ms",
                               (int)iClientIndex, (int)iPingLatency_ms, (int)pClient->iPingLatency_ms );
                             // Test results with ...
                             //  .. two instances running on the same machine / "localhost" :
                             //     round trip = 416, 314, 420, 294, 293, 297, 303 us, ...
                             //  .. two instances connected via mobile internet / domestic ADSL :
                             // round trip = 755, 1261, 190, 482, 603, 220,
                             //              237, 105,  207, 116, 132, 226 ms.
                             // (tested with one PC using a mobile phone as Mobile Hotspot,
                             //        the other PC using the domestic ADSL connection).
                             //
                             //  For comparison, two PCs connected via the "dynv6" test,
                             //   both using the same DSL/WLAN router (possibly "locally routed"):
                             // round trip = 29, 30,  5, 30, 20, 30, 27,
                             //              13, 19, 32, 47, 21,  7, 32 ms .
                             //   -> AUDIO BUFFERS must have a capacity
                             //      of several hundred milliseconds,
                             //      and the buffer usage must be dynamically
                             //      adjusted to pClient->iPingLatency_ms
                             //     (managed inside Remote_CW_Keyer\CwDSP.c).
                           }
                        }
                     }

                    // Assemble the SECOND RESPONSE (details above) :
                    pbPayload[0] = 2;                          // "2nd response"
                    pbPayload[1] = pClient->bCmdDataBuffer[1]; // "ping-ID"
                    pbPayload[2] = pClient->bCmdDataBuffer[2]; // future reserved / alignment dummy ..
                    pbPayload[3] = pClient->bCmdDataBuffer[3];
                    // Again, copy the THREE TIMESTAMPS into the new payload:
                    memcpy( pbPayload+4, pClient->i32PingTimestamp_ms, 3*4);
                    // And again, optionally show the NETWORK TRAFFIC (in human readable form, not as "hex dump" anymore)
                    if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                     &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
                          ||(pClient->nPeriodicMessagesInTrafficLog < 10 ) )                      // .. except for the first few messages
                      )
                     { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d Ping[%d] t1=%ld ms, t2=%ld ms, t3=%ld ms",
                         (int)iClientIndex, (int)pbPayload[0],
                         (long)CwNet_ReadNonAlignedInt32( pbPayload+4 ),
                         (long)CwNet_ReadNonAlignedInt32( pbPayload+8 ),
                         (long)CwNet_ReadNonAlignedInt32( pbPayload+12) );
                       if(  pClient->nPeriodicMessagesInTrafficLog < 32767 )
                        { ++pClient->nPeriodicMessagesInTrafficLog;
                        }
                     }
                  } // end < successfully appended the SECOND RESPONSE in the network buffer > ?
                 break; // end case CWNET_CMD_PING response 1
              case 2 : // the peer (usually a SERVER) sent the SECOND RESPONSE, so evaluate the final results.
                 // No response to send here. After this, BOTH peers know the turn-around times (~latency).
                 // Note: 'case 2' with "client #0" will be seen on the CLIENT SIDE.
                 if(  ( pClient->bCmdDataBuffer[1] == (BYTE)iClientIndex ) // "ID" as expected, or ..
                   || ( iClientIndex == CWNET_LOCAL_CLIENT_INDEX ) ) // .. this is the CLIENT side ?
                  {
                    // Store the THIRD 32-bit timestamp for later ( = the "second responder's" high-res-timestamp):
                    memcpy( &pClient->i32PingTimestamp_ms[2], pClient->bCmdDataBuffer+12, 4);
                    //  ,-------------------------------------|________________________|
                    //  '--> See sketches and purpose of these THREE TIMESTAMPS
                    //       in the CWNET_CMD_PING message further above in CwNet_ExecuteCmd().
                    iPingLatency_ms = pClient->i32PingTimestamp_ms[2]-pClient->i32PingTimestamp_ms[0];
                    if( iPingLatency_ms >= pClient->iPingLatency_ms ) // latency now HIGHER ->
                     { // let pClient->iPingLatency_ms jump to the new peak
                       pClient->iPingLatency_ms = iPingLatency_ms;
                     }
                    else // ping latency now lower -> let pClient->iPingLatency_ms DROP SLOWLY
                     { pClient->iPingLatency_ms -= (pClient->iPingLatency_ms-iPingLatency_ms) / 10;
                     }
                    if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
                     { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Ping test, client #%d: %d ms, pk = %d ms",
                         (int)iClientIndex, (int)iPingLatency_ms, (int)pClient->iPingLatency_ms );
                     }
                  }
                 break; // end case CWNET_CMD_PING response 2
              default: // oops.. there seem to other sub-commands which we don't understand yet
                 break;
            } // end switch( pClient->bCmdDataBuffer[0] )
         } // end if < well-formed "PING" >
        break; // end case pClient->iCurrentCommand == CWNET_CMD_PING
     case CWNET_CMD_PRINT  : // RECEIVED some "text to print" ...
        if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
         { INET_DumpTraffic_HexOrASCII( sz255, 100, pClient->bCmdDataBuffer, pClient->iCmdDataLength );
           ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d Print %s", (int)iClientIndex, sz255 );
         }
        // .. which also acts as the "log-in confirmation" for the client:
        if( (iClientIndex==CWNET_LOCAL_CLIENT_INDEX) && (pClient->iClientState==CWNET_CLIENT_STATE_LOGIN_SENT) )
         {  pClient->iClientState =  CWNET_CLIENT_STATE_LOGIN_CFMD;
            if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
             { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Client: Server confirmed log-in and sent:" );
             }
         } // end if < CWNET_CMD_PRINT received as log-in confirmation > ?
        // We already have the thread-safe ShowError() to emit DEBUG info,
        // so use this as primitive built-in chat feature :
        ShowError( ERROR_CLASS_INFO, "> %s", (char*)pClient->bCmdDataBuffer );
        //    (the above also prints the text into THE SERVER's 'Debug' tab,
        //     or whatever a more advanced GUI will use instead).
        if( iClientIndex != CWNET_LOCAL_CLIENT_INDEX )  // the "text to print" arrived at the SERVER ->
         { // Maybe send it back to ALL OTHER CLIENTS that are currently logged in ?
           // For those 'other clients' (excluding the one who SENT the message),
           // we cannot simply append the received text into pCwNet->bTxBuffer[] !
         } // end if < CWNET_CMD_PRINT arrived at the SERVER ? >
        break; // end case pClient->iCurrentCommand == CWNET_CMD_PRINT
     case CWNET_CMD_TX_INFO: // end of a block with "info about the TRANSMITTING client" ..
        if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
         { INET_DumpTraffic_HexOrASCII( sz255, 100, pClient->bCmdDataBuffer, pClient->iCmdDataLength );
           ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d Tinf %s", (int)iClientIndex, sz255 );
         }
        if( (pClient->iCmdDataLength >= 2) && (pClient->iCmdDataLength <= 80) )
         { strncpy( pClient->sz80TxInfo, (char*)pClient->bCmdDataBuffer+1, 80 );
           pClient->iAnnouncedTxClient = (int)(signed char)(pClient->bCmdDataBuffer[0]);
           // ,------'
           // '--> The GUI will notice the change in pCwNet->Client[0].iAnnouncedTxClient,
           //      and soon show "who's on the key now" (ideally with a callsign) .
           //      A NEGATIVE "client index" means "-- nobody --" has the key,
           //      so rush and take it !
           //
         }
        break; // end case pClient->iCurrentCommand == CWNET_CMD_TX_INFO
     case CWNET_CMD_MORSE  : // end of a block of MORSE CODE PATTERNS (not ASCII)
        // Nothing to do here, because EVERY SINGLE BYTE (key-up/key-down command
        // with a seven-bit timestamp) has IMMEDIATELY been queued up for
        // to the keyer thread via pCwNet->MorseRxFifo in CwNet_OnReceive().
        // NO BREAK ! // break; // end case pClient->iCurrentCommand == CWNET_CMD_MORSE
     case CWNET_CMD_AUDIO  : // end of a block of 8-bit A-Law compressed Audio samples
        // Nothing to do here as well, and "don't waste bandwidth in the DEBUG log" with audio messages
        // NO BREAK ! // break; // end case pClient->iCurrentCommand == CWNET_CMD_AUDIO
     case CWNET_CMD_SERIAL_PORT_TUNNEL_ANY: // end of any block with payload from a "Serial Port Tunnel"...
     case CWNET_CMD_SERIAL_PORT_TUNNEL_1:   // .. all not handled HERE,
     case CWNET_CMD_SERIAL_PORT_TUNNEL_2:   // .. for reasons explained BELOW !
     case CWNET_CMD_SERIAL_PORT_TUNNEL_3:   // (payload processed "immediately")
        // Nothing to do here, because EVERY SINGLE BYTE in the block has already been
        //  processed *immediately* - see CwNet_OnReceive(), case CWNET_CMD_SERIAL_PORT_TUNNEL_ ..
        // Despite that, now that we have received the COMPLETE block, show it in the log ?
        if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
         &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
              ||(pClient->nPeriodicMessagesInTrafficLog < 20 ) ) // .. except for the first few messages
          )
         { ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "Client%d: End cmd %s (0x%02X)",
                (int)iClientIndex,
                (char*)CwNet_CommandFromBytestreamToString( pClient->iCurrentCommand ),
                (unsigned int)pClient->iCurrentCommand );
           if(  pClient->nPeriodicMessagesInTrafficLog < 32767 )
            { ++pClient->nPeriodicMessagesInTrafficLog;
            }
         }
        break; // end case pClient->iCurrentCommand == CWNET_CMD_AUDIO
     case CWNET_CMD_CI_V   : // end of a CI-V packet (passed to/from Icom transceivers)
        if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
         { INET_DumpTraffic_HexOrASCII( sz255, 100, pClient->bCmdDataBuffer, pClient->iCmdDataLength );
           ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d CI-V %s", (int)iClientIndex, sz255 );
         }
        break; // end case CWNET_CMD_CI_V

     case CWNET_CMD_SPECTRUM: // end of a spectrum (from remote server to client)..
        // (Spectra don't travel as CI-V packets over the CW-Network's TCP socket,
        //  but in a simplified form beginning with a binary T_CwNet_SpectrumHeader,
        //  and followed by as many frequency bins as delivered by the remote radio.
        if( pClient->iCmdDataLength > sizeof(T_CwNet_SpectrumHeader) )
         { T_RigCtrl_Spectrum *pSpectrum;
           T_CwNet_SpectrumHeader spectrumHeader;
           BYTE *pbSource = pClient->bCmdDataBuffer;
           // memcopy the received block into a T_CwNet_SpectrumHeader as a local variable,
           // because the source (pClient->bCmdDataBuffer[0..pClient->iCmdDataLength-1])
           // may not be properly aligned to safely access 32- or 64-bit-members
           // on certain processors or controllers (e.g. ARM) :
           memcpy( &spectrumHeader, pbSource, sizeof(T_CwNet_SpectrumHeader) );
           pbSource += sizeof(T_CwNet_SpectrumHeader);
           // Only "accept" the received spectrum if the number of frequency bins
           // (indicated in the T_CwNet_SpectrumHeader) matches the received command's
           // block length:
           if( (spectrumHeader.u16NumBinsUsed > 0 )
            && (spectrumHeader.u16NumBinsUsed <= RIGCTRL_MAX_FREQ_BINS_PER_SPECTRUM)
            && (spectrumHeader.u16NumBinsUsed == (WORD)(pClient->iCmdDataLength-sizeof(T_CwNet_SpectrumHeader)))
             )
            { // Seems to be valid, so add to FIFO as if the spectrum was received
              // via RigControl.c from a "locally connected" radio (via USB):
              if( (pSpectrum=RigCtrl_AppendSpectrumToFIFO(&pCwNet->RigControl,
                                  spectrumHeader.flt32BinWidth_Hz,
                                  spectrumHeader.flt32FreqMin_Hz,
                                  spectrumHeader.u16NumBinsUsed)) != NULL )
               { // Ok, RigControl.c has added a new FIFO entry, so copy the
                 // frequency bins directly into the pSpectrum (in the FIFO):
                 n = spectrumHeader.u16NumBinsUsed;
                 for( i=0; i<n; ++i )
                  { pSpectrum->fltMagnitudes_dB[i] = (float)(signed char)(*pbSource++);
                  }
               } // end if < RigCtrl_AppendSpectrumToFIFO() successful ? >
            }   // end if < plausible "number of frequency bins" versus received block-length> ?
         }     // end if < block length sufficient for a T_CwNet_SpectrumHeader > ?
        break; // end case CWNET_CMD_SPECTRUM

     case CWNET_CMD_FREQ_REPORT: // Transceiver's frequency, mode, etc  (T_CwNet_VfoReport with ONE 'variable' parameter)
        if( pClient->iCmdDataLength == sizeof(T_RigCtrl_VfoReport) )
         { T_RigCtrl_VfoReport *pVfoReport = (T_RigCtrl_VfoReport*)pClient->bCmdDataBuffer;
           // '--> no problem with alignment because bCmdDataBuffer IS on an 8-byte-boundary,
           //      per design of the T_CwNetClient struct (actually,
           //      BYTE bCmdDataBuffer[CWNET_CMD_DATA_BUFFER_SIZE] it's the first struct member,
           //                      and CWNET_CMD_DATA_BUFFER_SIZE is a power of two.
           //
           if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
            &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
                 ||(pClient->nPeriodicMessagesInTrafficLog < 20 ) ) // .. except for the first few messages
             )
            { ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d FreqRpt f=%.1lf kHz, txStat=%d, S-meter=%d dB",
                (int)iClientIndex,
                (double)(pVfoReport->dblVfoFrequency*1e-3),
                (int)pVfoReport->bTrxStatus, // +1 = transmitting, +2=audio muted, etc
                (int)pVfoReport->i8SMeterLevel_dB );
              if(  pClient->nPeriodicMessagesInTrafficLog < 32767 )
               { ++pClient->nPeriodicMessagesInTrafficLog;
               }
            }
           if( pCwNet->cfg.iFunctionality == CWNET_FUNC_CLIENT )
            { // THIS instance runs as a client, so the VFO report was sent by the remote server:
              RigCtrl_OnVfoReportFromNetwork( &pCwNet->RigControl, pVfoReport );
              // '--> Only stores the new VFO frequency (etc) somewhere,
              //      because the CLIENT isn't controlling a 'local' radio.
            }
           else if( pCwNet->cfg.iFunctionality == CWNET_FUNC_SERVER )
            { // THIS instance runs as a server, so the VFO report was sent by a remote client.
              // Only "apply" the new setting in the locally connected radio
              // if the remote client has the permission to CONTROL the radio:
              if( pClient->iPermissions & CWNET_PERMISSION_CTRL_RIG )
               { RigCtrl_OnVfoReportFromNetwork( &pCwNet->RigControl, pVfoReport );
               }
            }
           // After receiving a new frequency from the remote peer,
           // avoid sending back a 'Request to "QSY"' to the same frequency:
           pClient->dblReportedVfoFrequency = pCwNet->RigControl.dblVfoFrequency;
         }
        else
         { ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d FreqReport: ILLEGAL BLOCK SIZE (%d instead of %d bytes)",
             (int)iClientIndex, (int)pClient->iCmdDataLength, (int)sizeof(T_RigCtrl_VfoReport) );
         }
        break; // end case CWNET_CMD_FREQ_REPORT

     case CWNET_CMD_PARAM_INTEGER: // received a single parameter, data type 'integer',
        // identified by RIGCTRL_PN_xyz, wrapped in a  T_RigCtrl_ParamReport_Integer
      { T_RigCtrl_ParamReport_Integer *pReport = (T_RigCtrl_ParamReport_Integer*)pClient->bCmdDataBuffer;
        T_RigCtrl_ParamInfo *pPI = RigCtrl_GetInfoForUnifiedParameterNumber(pReport->bRigControlParameterNumber);
        if( pPI != NULL )
         { if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // occasionally sent, no "CPU hog"..
            { if( pPI->iUnifiedPN == RIGCTRL_PN_OP_MODE ) // 'operation mode' (USB/LSB/CW..) ? Show as a STRING, not a NUMBER
               { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP,
                    "RX%d IntPara %s = %s", (int)iClientIndex, pPI->pszToken,
                   RigCtrl_OperatingModeToString( (int)pReport->i32ParameterValue ) );
               }
              else // "nothing special" so show the parameter value in numeric form
               { ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP,
                  "RX%d IntPara %s = %ld %s", (int)iClientIndex, pPI->pszToken,
                  (long)pReport->i32ParameterValue, pPI->pszUnit );
               }
            } // end if < show the 'integer parameter' report in the traffic log > ?
           RigCtrl_OnParamReport_Integer( &pCwNet->RigControl, pReport  );
           // Example: Remote client switches the operating mode ("OpMode") to CW:
           //   -> RigCtrl_OnParamReport_Integer() calls RigCtrl_SetParamValue_Int(),
           //      RigCtrl_SetParamValue_Int() recognizes a CHANGE in the value,
           //      RigCtrl_QueueUpCmdToWriteUnifiedPN() does what the name implies,
           //      RigCtrl_OnParamReport_Integer() returns IMMEDIATELY without waiting for anything.
           // LATER, in RigControl.c : RigCtrl_Handler(), RigCtrl_SendReadWriteCommandFromFIFO()
           //  pulls the "unified parameter number" (e.g. pPI->iUnifiedPN = 3 = RIGCTRL_PN_OP_MODE)
           //  from the queue of to-be-written parameters again, and  
           //  RigCtrl_SendWriteCommandForUnifiedPN()
         }
      } break; // end case CWNET_CMD_PARAM_INTEGER

     case CWNET_CMD_PARAM_DOUBLE: // a single parameter, data type 'double',
        // identified by RIGCTRL_PN_xyz, wrapped in a  T_RigCtrl_ParamReport_Double
      { T_RigCtrl_ParamReport_Double *pReport = (T_RigCtrl_ParamReport_Double*)pClient->bCmdDataBuffer;
        T_RigCtrl_ParamInfo *pPI = RigCtrl_GetInfoForUnifiedParameterNumber(pReport->bRigControlParameterNumber);
        if( pPI != NULL )
         { if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // occasionally sent, no "CPU hog"..
            { ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP,
                  "RX%d DblPara %s = %lf %s", (int)iClientIndex, pPI->pszToken,
                  (double)pReport->dblParameterValue, pPI->pszUnit );
            } // end if < show the 'double parameter' report in the traffic log > ?
           RigCtrl_OnParamReport_Double( &pCwNet->RigControl, pReport  );
         }
      } break; // end case CWNET_CMD_PARAM_DOUBLE

     case CWNET_CMD_PARAM_STRING: // a single parameter, data type 'string',
        // identified by RIGCTRL_PN_xyz, wrapped in a  T_RigCtrl_ParamReport_String ?
        RigCtrl_OnParamReport_String( &pCwNet->RigControl, (T_RigCtrl_ParamReport_String*)pClient->bCmdDataBuffer );
        break; // end case CWNET_CMD_PARAM_STRING

  // case CWNET_CMD_QUERY_PARAM:  // request to send a certain parameter, identified by RIGCTRL_PN_xyz
  //    // ... future plan ...
  //    break; // end case CWNET_CMD_QUERY_PARAM

     case CWNET_CMD_MULTI_FUNCTION_METER_REPORT: // received a group of "meter readings" to emulate Icom's "Multi-Function Meter"
        if( pClient->iCmdDataLength >= 8/*! .. not sizeof(T_RigCtrl_MultiFunctionMeterReport)*/ )
         { T_RigCtrl_MultiFunctionMeterReport* pMeterReport = (T_RigCtrl_MultiFunctionMeterReport*)pClient->bCmdDataBuffer;
           if( pCwNet->cfg.iFunctionality == CWNET_FUNC_CLIENT )
            { // THIS instance runs as a client, so the "multi-function meter" report was sent by the remote server:
              RigCtrl_OnMultiFunctionMeterReportFromNetwork( &pCwNet->RigControl, pMeterReport );
              // '--> Only stores the new 'meter readings' (power, ALC, compressor, supply voltage, drain current),
              //      because the CLIENT isn't controlling a 'local' radio.
            }
           else if( pCwNet->cfg.iFunctionality == CWNET_FUNC_SERVER )
            { // THIS instance runs as a server, so the "meter readings" were sent by a remote client ?!
              // Only "apply" the new setting in the locally connected radio
              // if the remote client has the permission to CONTROL the radio:
              if( pClient->iPermissions & CWNET_PERMISSION_CTRL_RIG )
               { RigCtrl_OnMultiFunctionMeterReportFromNetwork( &pCwNet->RigControl, pMeterReport );
               }
            }
           if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
            &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
                 ||(pClient->nMeterReportsInTrafficLog < 3 ) ) // .. except for the first few messages
             )
            { RigCtrl_MultiFunctionMeterReportToString( pMeterReport, sz255, 80 );
              ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d MeterRp %s", (int)iClientIndex, sz255 );
              if(  pClient->nMeterReportsInTrafficLog < 32767 )
               { ++pClient->nMeterReportsInTrafficLog;
               }
            } // end if < show the "multi function meter" report in the traffic log > ?
         }
        break; // end case CWNET_CMD_MULTI_FUNCTION_METER_REPORT

     case CWNET_CMD_POTI_REPORT: // a group of "poti settings", exchanged less frequently than the "meter report"
        if( pClient->iCmdDataLength >= 8/*! .. not sizeof(T_RigCtrl_MultiFunctionMeterReport)*/ )
         { T_RigCtrl_PotiReport* pPotiReport = (T_RigCtrl_PotiReport*)pClient->bCmdDataBuffer;
           if( pCwNet->cfg.iFunctionality == CWNET_FUNC_CLIENT )
            { // THIS instance runs as a client, so the "poti setting" report was sent by the remote server:
              RigCtrl_OnPotiReportFromNetwork( &pCwNet->RigControl, pPotiReport );
              // '--> Only stores the new 'potentiometer settings' (RF power setting, etc etc),
              //      because the CLIENT isn't controlling a 'local' radio.
            }
           else if( pCwNet->cfg.iFunctionality == CWNET_FUNC_SERVER )
            { // THIS instance runs as a server, so the "poti settings" were sent by a remote client.
              // Only "apply" the new settings in the locally connected radio
              // if the remote client has the permission to CONTROL the radio:
              if( pClient->iPermissions & CWNET_PERMISSION_CTRL_RIG )
               { RigCtrl_OnPotiReportFromNetwork( &pCwNet->RigControl, pPotiReport );
               }
            }
           if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // rarely sent, thus not a "CPU hog"..
            { RigCtrl_PotiReportToString( pPotiReport, sz255, 80 );
              ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d PotiRpt %s", (int)iClientIndex, sz255 );
              if(  pClient->nMeterReportsInTrafficLog < 32767 )
               { ++pClient->nMeterReportsInTrafficLog;
               }
            } // end if < show the "poti report" in the traffic log > ?
         }
        break; // end case CWNET_CMD_POTI_REPORT

     case CWNET_CMD_BAND_STACKING_REGISTER: // a "Band Stacking Register", converted into a string of key=value pairs by RigCtrl_FreqMemEntryToString()
        if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
         { INET_DumpTraffic_HexOrASCII( sz255, 100, pClient->bCmdDataBuffer, pClient->iCmdDataLength-1 );
           ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d BandStk %s", (int)iClientIndex, sz255 );
         }
        if( pClient->iCmdDataLength > 0 )/* <- here, the "Data" are a string of key=value pairs, comma separated.. don't speculate about the length */
         { if( pCwNet->cfg.iFunctionality == CWNET_FUNC_CLIENT )
            { // THIS instance runs as a client, so the "Band-Stacking Register" was sent by the remote server:
              RigCtrl_OnBandStackingRegisterReportFromNetwork( &pCwNet->RigControl, (char*)pClient->bCmdDataBuffer );
              // '--> Only parses the new "Band Stacking Register" (one of many entries),
              //      and stores it in an array of structs in the 'RigControl' instance.
              //      The REMOTE CW KEYER (GUI application) will show the result
              //      in the 'band list' on the TRX tab. Details in the implementation.
            }
           else if( pCwNet->cfg.iFunctionality == CWNET_FUNC_SERVER )
            { // THIS instance runs as a server, so the "Band-Stacking Register" was sent by a remote client ?!
              // Do we really want to let him write that into our transceiver ? Guess not, at least not for a start.
            }
         }
        break; // end case CWNET_CMD_BAND_STACKING_REGISTER

     case CWNET_CMD_USER_DEFINED_BAND: // a "User Defined Band", converted into a string of key=value pairs by RigCtrl_BandDefinitionToString()
        if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
         { INET_DumpTraffic_HexOrASCII( sz255, 100, pClient->bCmdDataBuffer, pClient->iCmdDataLength );
           ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d UserBand %s", (int)iClientIndex, sz255 );
         }
        if( pClient->iCmdDataLength > 0 )/* <- here, the "Data" are a string of key=value pairs, comma separated.. don't speculate about the length */
         { if( pCwNet->cfg.iFunctionality == CWNET_FUNC_CLIENT )
            { // THIS instance runs as a client, so the user-defined band definition was sent by the remote server:
              RigCtrl_OnUserDefinedBandReportFromNetwork( &pCwNet->RigControl, (char*)pClient->bCmdDataBuffer );
              // '--> Parses the new "User Defined Band" (one of many entries),
              //      and stores it in an array of structs in the 'RigControl' instance.
            }
           else if( pCwNet->cfg.iFunctionality == CWNET_FUNC_SERVER )
            { // THIS instance runs as a server, so the "User Defined Band" was sent by a remote client,
              // and is ignored here for safety. Only the server's sysop can modify
              // the list of supported bands, e.g. by editing RCWKeyer_Bands.txt .
            }
         }
        break; // end case CWNET_CMD_USER_DEFINED_BAND

     case CWNET_CMD_RIGCTLD : // 0x06 : 'rigctld'-compatible ASCII string (plus a trailing ZERO) - see www.mankier.com/1/rigctld .
        // So far, we use this for PTT CONTROL and other not-so-frequent commands, where the 'rigctld'-ASCII-overhead doesn't hurt so much
        if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
         { INET_DumpTraffic_HexOrASCII( sz255, 100, pClient->bCmdDataBuffer, pClient->iCmdDataLength-1/*omit the trailing zero*/ );
           ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d Rigctld %s", (int)iClientIndex, sz255 );
         }
        n = CwNet_ExecuteRigctldCmd( pCwNet, pClient,
                 (char*)pClient->bCmdDataBuffer, // [in] Rigctrld/Hamlib compatible command string
                 sz255, 250 ); // [out] string to send as a response
        if( n>0 ) // send back a response, e.g. "RPRT 0\n" ?
         { CwNet_SendRigctldCompatibleString( pCwNet, pClient, sz255 );
           // '--> includes the optinal display on the 'Debug' tab via ShowError()
           // Result e.g.: > 18:24:51.5 RX1 Rigctld [0009] set_ptt 1
           //              > 18:26:09.6 TX1 Rigctld [0007] RPRT 0\n  (that's only the payload, less the THREE-BYTE-HEADER and the string terminator)
           // ,--------------------------------------|__|
           // '--> This is what Hamlib calls "end of the response marker".
           //      Still not a clue what "RPRT x" means ... "report" ?
           //      Anyway, a negative value of "x" is an ERROR, and x = 0 is "ok".
         }
        break; // end case CWNET_CMD_RIGCTLD

     default : // silently ignore unknown commands for 'future compatibility', except.. :
        if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
         &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
              ||(pClient->nUnknownMessagesInTrafficLog < 20 ) ) // .. except for the first few messages
          )
         { ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "Client%d: End cmd %s (0x%02X)",
                (int)iClientIndex,
                (char*)CwNet_CommandFromBytestreamToString( pClient->iCurrentCommand ),
                (unsigned int)pClient->iCurrentCommand );
           if(  pClient->nUnknownMessagesInTrafficLog < 32767 )
            { ++pClient->nUnknownMessagesInTrafficLog;
            }
         }
        break;
   } // end switch( pClient->iCurrentCommand )

  (void)fOk;

  i = CwNet_SanityCheck( pCwNet ); // <- built-in self-text: Did CwNet_ExecuteCmd() trash our struct ?
  if( i != 0 )
   { ShowError( ERROR_CLASS_FATAL | SHOW_ERROR_TIMESTAMP, "CwNet: Sanity check failed (code %d)", (int) i );
   }
} // end CwNet_ExecuteCmd()

//---------------------------------------------------------------------------
T_CwNet_CmdTabEntry *CwNet_FindRigctlCommand( const char **ppszSrc )
{ T_CwNet_CmdTabEntry *pCmd = (T_CwNet_CmdTabEntry*)CwNet_Rigctld_Commands;
  const char *cp, *cpSrc = *ppszSrc;
  const char *cpSrcEndstop = cpSrc + SL_strnlen( cpSrc, 16/*iMaxLen*/ );
  char  c;
  int   n;
  BOOL fLongCmd = FALSE;

  if( *cpSrc == '\\' ) // seems to be a "long command".. how long is THE TOKEN ?
   { ++cpSrc;          // skip the leading backslash
     fLongCmd = TRUE;
   }
  else // THIS implementation doesn't need an ugly leading backslash
   {   // to recognize a token as a "long command" !
     if( SL_IsLetter(cpSrc[0]) && SL_IsLetter(cpSrc[1]) )
      { fLongCmd = TRUE;
      }
   }

  if( fLongCmd )
   { cp = cpSrc;
     while( (cp<cpSrcEndstop) && SL_IsLetterOrUnderscore(*cp) )
      { ++cp;
      }
     n = cp - cpSrc;  // length of the "long command" for string comparison,
                      // e.g. bRcvdLine = "\\chk_vfo\n" -> n = 7
     while( pCmd->pszLongToken != NULL ) // repeat until found or end-of-table
      { if(  (SL_strncmp( cpSrc, pCmd->pszLongToken, n ) == 0 )
           &&(pCmd->pszLongToken[n] == '\0') ) // token length also ok ?
         { *ppszSrc = cpSrc+n; // skip the recognized token in the caller's source pointer
           return pCmd;        // e.g. found "set_ptt" (with n=7) in "set_ptt 1"
         }
        else
         { ++pCmd;    // check the next entry in CwNet_Rigctld_Commands[]
         }
      }
   }
  else // not a "long command" but a single byte (not necessary a plain text character) :
   { while( pCmd->pszLongToken != NULL ) // repeat until found or end-of-table
      { if( *cpSrc == pCmd->cShortToken )
         { *ppszSrc = cpSrc+1; // skip the recognized single-character token in the caller's source pointer
           return pCmd;
         }
        else
         { ++pCmd;    // check the next entry in CwNet_Rigctld_Commands[]
         }
      }
   }
  return NULL; // arrived here ? command not recognized; leave *ppszSrc unchanged
} // end CwNet_FindRigctlCommand()

//---------------------------------------------------------------------------
int CwNet_CheckCommandType( const char *pszCmd ) // checks in advance if the string
     // is a "rigctl[d]"-compatible command line, "short" or "long" form.
     // Unlike CwNet_ExecuteRigctldCmd(), doesn't *execute* the command.
     // Return values: CWNET_CMDTYPE_NONE = "not recognized",
     //                CWNET_CMDTYPE_RIGCTRL_SET = it's a rigctl-like "set" command,
     //                CWNET_CMDTYPE_RIGCTRL_GET = it's a rigctl-like "get" command.
{
  T_CwNet_CmdTabEntry *pCmd = CwNet_FindRigctlCommand(&pszCmd);
  if( pCmd != NULL )
   { if( pCmd->iAccess == HLSRV_ACCESS_SET ) // HLSRV_ACCESS_GET or HLSRV_ACCESS_SET ?
      { return CWNET_CMDTYPE_RIGCTRL_SET;
      }
     else
      { return CWNET_CMDTYPE_RIGCTRL_GET;
      }
   }
  return CWNET_CMDTYPE_NONE;
} // end CwNet_CheckCommandType() [of a "Hamlib" / "rigctl[d]" compatible command]


//---------------------------------------------------------------------------
int CwNet_ExecuteRigctldCmd( T_CwNet *pCwNet, T_CwNetClient *pClient,
                        const char *pszCmd, // [in] received command
                              char *pszResp, int iMaxRespLength) // [out] response (string)
  // [in]  pszCmd[ 0..iLength-1 ]
  //         (for easier parsing, already terminated like a C-string)
  // [out] pszResp[ 0..iMaxRespLength-1 ]
  // [return] NUMBER OF BYTES actually placed in pszResp[], without the trailing zero;
  //          or a NEGATIVE-made error code like -RIGCTRL_ERROR_NOT_IMPLEMENTED .
  //
  // Called from CwNet_ExecuteCmd() after receiving a zero-terminated C string
  //        with a 'rigctld'-compatible command line.
  // Also called from the Remote CW Keyer GUI (Keyer_GUI.cpp : KeyerGUI_ExecuteCommand)
  //        when the local operator entered a 'rigctl[d]'-compatible command,
  //        but without the need for a BACKSLASH before a "long command",
  //        in contrast to the original 'hamlib' command specification.
  //
  // Details about the simple format used to be at https://www.mankier.com/1/rigctld .
  // See also: cbproj\Remote_CW_Keyer\HamlibServer.c (which ONLY supports
  //           the old 'hamlib' compatible commands, and thus completely
  //           lacks all features of CwNet.c like ...
  //              * sharing audio between server/radio and multiple clients
  //              * sending Morse code the way the OPERATOR has sent it
  //              * sharing the spectrum / spectrogram display
  //                 (at least in 2024, there wasn't a trace of anything similar
  //                  in the original 'rigctld' documentation)
  //  * THE "HAMLIB SERVER" IS AN OPTIONAL, AND HIGHLY EXPERIMENTAL FEATURE
  //    in the 'windows GUI appliation' for DL4YHF's Remote CW Keyer,
  //    so we don't want any dependency on HamlibServer.c in CwNet.c !
  // From a "manpage" about the original Hamlib "rigctld":
  // > Commands can be sent over the TCP socket either as a single char,
  // > or as a long command  name plus the value(s) space separated on one
  // > '\n' terminated line. See PROTOCOL.
  // >
  // > Since most of the Hamlib operations have a set and a get method,
  // > an upper case letter will be used for set methods
  // > whereas the corresponding lower case letter refers to the get method.
  // > Each operation also has a long name; prepend a backslash to send a
  // > long command name.
  //
  // Typical call stack :
  //  ServerThread() -> CwNet_OnReceive() -> CwNet_ExecuteCmd() -> CwNet_ExecuteRigctldCmd() .
{
  // int  iClientIndex = (int)(pClient - &pServer->Client[0]); // again the "pointer trick" to get an array index (in C)
  int  i,n;
  int  iPingLatency_ms;
  BYTE *pb, *pbCommandBlockStart;
  const char *cp;
  const char *cpSrc        = pszCmd;
  char       *cpDest       = pszResp;
  const char *cpDstEndstop = pszResp + iMaxRespLength;
  int  iOldNumBytesInTxBuffer;
  int  iResult, iResponseLength = 0;
  T_CwNet_CmdTabEntry *pCmd;

  if( (cpDstEndstop-cpDest) < 16 ) // that's not enough space even for a "short response" like "RPRT 0\n" (whatever that means)
   { return -RIGCTRL_ERROR_OUT_OF_MEMORY;  // bail out (nothing in pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ] yet)
   }

  // This "liberal" implementation doesn't need an ugly leading backslash
  // to recognize a token as a "long command", so:
  SL_SkipChar( &cpSrc, '\\' ); // just SKIP this 'prefix for a "long command" - our parser doesn't need it
                               // (on this occasion: It also doesn't need a "\n" at the end of the command.
                               //  The usual string terminator, "\0", works as well.
                               //  This feature is important because the SINGLE-LINE
                               //  text editor in the GUI's status line will not
                               //  insert "\n" or "\r" into the text at all.
  pCmd = CwNet_FindRigctlCommand(&cpSrc);  // <- skips the command when successful

  if( pCmd != NULL )  // found the Hamlib command in the lookup table ?
   { // If the handler appends NOTHING but returns e.g. 0 = HAMLIB_RESULT_NO_ERROR,
     //    CwNet_ExecuteRigctldCmd() appends the "RPRT x"-thing,
     //    > where x is the numeric return value of the Hamlib backend function
     //    > that was called by the command.
     // If there's a "handler" for this get-/set-/check-command, invoke it:
     if( pCmd->pHandler != NULL )
      { pszResp[0] = '\0';  // just in case the 'command handler' doesn't set this..
#      ifndef __CODEGEARC__
        iResult = pCmd->pHandler( pCwNet, pClient, // -> e.g. CwNet_Rigctld_OnGetFreq() 
             pCmd, cpSrc/*arguments*/,   // [in] e.g. "\n" if there are NO arguments in the line.
             pszResp, iMaxRespLength );  // [out] response string (with a limited length)
        // The above function pointer call was ok for BCB V6, no warnings.
        // But C++Builder V12 (using the "Clang-enhanced compiler") complained:
        // > [bcc32c Warning] : incompatible pointer types passing
        // >     'T_CwNet_CmdTabEntry *' (aka 'struct t_CwNet_CmdTabEntry *')
        // >  to parameter of type
        // >     'struct t_CwNet_CmdTabEntry *' .
        // What the heck ? Both are THE SAME STRUCTURE, once as "struct Blah",
        //                                          and once as "type Blah",
        //  which was used this way because of the classic 'chicken and egg' problem
        //  where a struct contains a function pointer, and the function itself
        //  expects the address of the struct itself (kind of "this"-pointer w/o C++).
#      else //  __CODEGEARC__ defined -> try to eliminate the umpteenth warning..
        iResult = pCmd->pHandler( pServer, pClient,
          (struct t_CwNet_CmdTabEntry*)pCmd, // <- OMG. What an ugly kludge for a pedantic dumb compiler ! (see the stupid warning further below)
             pCmd, cpSrc/*arguments*/,       // [in] e.g. "\n" if there are NO arguments in the line.
             pszResp, iMaxRespLength );  // [out] response string (with a limited length)
          // With the above cast, the WARNING from "bcc32x" (C++Builder V12)
          // was even more ridiculous :
          // > [bcc32c Warning] : incompatible pointer types passing
          // >      'struct t_CwNet_CmdTabEntry *' to parameter of type
          // >      'struct t_CwNet_CmdTabEntry *' .
          // Where is the difference between these two pointer types, Mr. Pedantic ?
#      endif // Extrawurst für 'Embarcadero' / 'Codegear' C++Builder V12 / "bcc32c" ?
        iResponseLength = SL_strnlen( pszResp, iMaxRespLength ); // plus one to send the trailing zero, too
        if( iResponseLength <= 1 ) // oops.. the function called via pointer did NOT provide a response ..
         { cpDest = pszResp;
           SL_AppendPrintf( &cpDest, cpDstEndstop, "RPRT %d\n", (int)iResult );
           // '--> Don't emit this via ShowError() for the debug log here,
           //      because THE CALLER (CwNet_ExecuteCmd) will do that.
           iResponseLength = SL_strnlen( pszResp, iMaxRespLength );
         } // end switch( iResult )
        ++iResponseLength; // send the trailing zero, too
      }   // end if < got a function pointer with the "command handler" >
   }     // end if < found the short or long command name > ?
  return iResponseLength;  // non-negative result -> SUCCESS (and the number of character placed in pszResp)
} // end CwNet_ExecuteRigctldCmd()

//---------------------------------------------------------------------------
BOOL CwNet_WriteStringToChatTxFifo( T_CwNet *pCwNet, T_CwNetClient *pClient, const char *pszTxData )
  // Thread-safe API function, called from the GUI when there's something to send
  // for the 'chat' (short text messages).
  // Intended to be used by the sysop for info like "Ant: dipole, Pwr: 5 watt" .
  // Returns TRUE when successfully written to the TX FIFO.
  // The actual transmission (merged into the TCP/IP stream) happens later.
  //  [out] pClient->sChatTxFifo : transmit-FIFO for the 'text chat'.
  //  [return] TRUE when successful, FALSE otherwise (e.g. out of FIFO space).
{
  int nBytesWritten, nBytesToWrite = strlen(pszTxData);

  // Either write the ENTIRE STRING or NOTHING AT ALL ! :
  if( nBytesToWrite <= CFIFO_GetNumBytesWriteable( &pClient->sChatTxFifo.fifo ) )
   { nBytesWritten = CFIFO_Write( &pClient->sChatTxFifo.fifo, (BYTE*)pszTxData,
                                   nBytesToWrite, 0.0/*dblTimestamp_s*/ );
     return ( nBytesWritten == nBytesToWrite );
   }
  return FALSE;
} // end CwNet_WriteStringToChatTxFifo()


//---------------------------------------------------------------------------
int CwNet_ReadStringFromChatRxFifo( T_CwNet *pCwNet, T_CwNetClient *pClient, char *pszRxData, int iMaxLen )
  // Counterpart to CwNet_WriteStringToChatTxFifo(). Details and purpose there.
  //  [in] pClient->sChatRxFifo : receive-FIFO for the 'text chat'.
  //  [return] number of characters actually placed in pszRxData .
  //           0 = "nothing received".
{
  int nBytesRead = CFIFO_Read( &pClient->sChatRxFifo.fifo, (BYTE*)pszRxData, iMaxLen-1, NULL/*pdblTimestamp_s*/ );
  if( (nBytesRead>=0 ) && (nBytesRead<iMaxLen) )
   { pszRxData[nBytesRead] = '\0'; // provide a trailing zero for the C-string
   }
  return nBytesRead; // .. WITHOUT the trailing zero appended above
} // end CwNet_ReadStringFromChatRxFifo()


#if( SWI_NUM_AUX_COM_PORTS > 0 )
//---------------------------------------------------------------------------
static void CwNet_PollForTransmissionFromAuxComPort(T_CwNet *pCwNet,
              T_CwNetClient *pClient) // [in] instance data for a single CLIENT
{
  //  If the space in the TX-buffer still permits at this point,
  //  send another chunk bytes from one of the 'Additional COM Ports',
  //  when configured as "Client/Server Tunnel" (that's the term used
  //  in the manual, see Remote_CW_Keyer.htm#Additional_COM_Ports).
  //  NOT required for additional COM ports with RIGCTRL_PORT_USAGE_VIRTUAL_RIG,
  //  because they are limited to what module RigControl.c 'understands'.
  int iLen = CwNet_GetFreeSpaceInTxBuffer(pCwNet); // <- number of BYTES ..
  int iAuxComPortIndex, iTunnelIndex, nBytesToRead;
  T_AuxComPortInstance *pAuxCom;
  BYTE bCwNetCommand;
  BYTE *pbPayload;
  if( iLen>256 )  // ok to send data from any serial port tunnel now ?
   {
     for( iAuxComPortIndex=0; iAuxComPortIndex<SWI_NUM_AUX_COM_PORTS; ++iAuxComPortIndex )
      { pAuxCom = &AuxComPorts[iAuxComPortIndex];
        if( pAuxCom->pRigctrlPort->iPortUsage == RIGCTRL_PORT_USAGE_SERIAL_TUNNEL )
         { iTunnelIndex = pAuxCom->iTunnelIndex;
           if( iTunnelIndex < 0 ) // tunnel that shall RECEIVE FROM "anywhere" ?
            {  bCwNetCommand = CWNET_CMD_SERIAL_PORT_TUNNEL_ANY;
            }
           else // normal "serial port tunnel", for a point-to-point connection:
            {  bCwNetCommand = CWNET_CMD_SERIAL_PORT_TUNNEL_1 + iTunnelIndex;
            }
           nBytesToRead = CFIFO_GetNumBytesReadableForTailIndex(
                               &pAuxCom->sRxFifo.fifo,
                                // ,----------------'
                                // '--> "Receive-FIFO" for AuxComThread() !
                                //      FILLED with data RECEIVED FROM a serial port,
                                //      and SENT here (in CwNet.c) via TCP/IP !
                                pClient->iTxBufferTailForAuxComPort[iAuxComPortIndex] );
           if( nBytesToRead > (iLen-64) )
            {  nBytesToRead =  iLen-64; // leave a few bytes of EMPTY SPACE
               // in this socket's TX buffer, because the smaller stuff
               // further below may want to be sent, too.
               // The 'additional COM ports' have sufficient buffers
               // so we can send their data a few milliseconds later.
            }
           if( nBytesToRead > 0 )  // ok, got something to send through THIS "serial port tunnel" ..
            { // (Sometimes with as little as ONE BYTE PAYLOAD, but we don't know
              //  when the NEXT byte arrives so send immediately - even a single byte)
              if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, bCwNetCommand, nBytesToRead ) ) != NULL )
               { CFIFO_ReadBytesForTailIndex(
                    &pAuxCom->sRxFifo.fifo, // source
                    pbPayload,              // destination
                    nBytesToRead,           // number of bytes to read === number of bytes ALLOCATED
                    &pClient->iTxBufferTailForAuxComPort[iAuxComPortIndex] ); // [in,out] int *piTailIndex );
                 // For some protocols (e.g. Yaesu's CAT protocol as used in FT-817),
                 // it's important to keep the original transmit timing,
                 // because (e.g. in Yaesu's poorly specified CAT protocol)
                 // bytes must be sent in 'packets' with only a few milliseconds
                 // between bytes of a frame, and larger inter-packet spaces
                 // must be preserved even when 'tunnelled' over TCP/IP.
                 // If the connection isn't in the local network but uses the
                 // internet, it's almost impossible to keep that timing
                 // *IF THE SERIAL TUNNEL ISN'T AWARE OF THE PROTOCOL*.
                 //        Using e.g. the stoneage "FT-817 Commander" talking to
                 //        the author's FT-817[ND] with RCW-Keyer's 'Serial Port Tunnel'
                 //        in between, and "com0com" on the CLIENT side,
                 //        the communication between the "Commander" and the FT-817
                 //        was very unreliable. Details about that, and the timing
                 //        of 5-byte-packets received at the FT-817 are in
                 //        AuxComPorts.c : AuxComThread() /
                 iLen = CwNet_GetFreeSpaceInTxBuffer(pCwNet); // update the free space in CwNet's TX BUFFER
                 // Example (with a terribly small circular serial-port-FIFOs in AuxComPorts.c):
                 // 256 bytes (thus maximum netto usage = 255 bytes), got here with
                 // nBytesToRead = 255 (oops.. pAuxCom->sRxFifo was COMPLETELY FULL),
                 // iLen = 15264 bytes BEFORE, and iLen = 15007 bytes AFTER the allocation.
                 //          '---- delta = 15264-15007 ----' = 257(!), ok, but we should send MUCH more in a block.
               }
            } // end if( nBytesToRead > 0 )
         }   // end if < RIGCTRL_PORT_USAGE_SERIAL_TUNNEL > ?
      }     // end for < iAuxComPortIndex >
   }       // end if < enough space in THIS socket's TX-buffer > ?
}         // end CwNet_PollForTransmissionFromAuxComPort()
#endif   // SWI_NUM_AUX_COM_PORTS > 0 ?


//---------------------------------------------------------------------------
static void CwNet_OnPoll( T_CwNet *pCwNet, // allows sending "unsolicited data"
              T_CwNetClient *pClient) // [in] instance data for a CLIENT:
  // [out, optional] pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ]
  //      = "anything to send", limited to
  //         CWNET_SOCKET_TX_BUFFER_SIZE - pCwNet->nBytesInTxBuffer (on entry)
  // Periodically called from either ClientThread() or ServerThread() to poll for
  // transmission, even if there was NOTHING RECEIVED on a socket during the
  // "polling interval" of approximately <CWNET_SOCKET_POLLING_INTERVAL_MS> .
  //
{
  int  iClientIndex = (int)(pClient - &pCwNet->Client[0]); // again the "pointer trick" to get an array index (in C)
       // '--> 0 = CWNET_LOCAL_CLIENT_INDEX, 1..N = indices into the server's table of REMOTE CLIENTS
  BYTE *pbPayload;
  int  i, iLen, iLen2, iHeadIndex, iTailIndex, nSamples, nSpectra;
  long  i32Time_ms;
  float fltSample;
  short i16Sample;
  char  sz255[256]; // temporary string buffer to find the required length
      // BEFORE allocating space in the microcontroller-friendly "network buffer"

  i32Time_ms = TIM_ReadStopwatch_ms( &pClient->swActivityWatchdog );
  if( i32Time_ms > CWNET_ACTIVITY_TIMEOUT_MS )
   { if( ! pClient->fDisconnect ) // only show this if NOT DISCONNECTED YET :
      { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Server%d: Disconnected %s:%d (timeout after %ld ms), rx=%ld, tx=%ld",
                (int)iClientIndex, CwNet_IPv4AddressToString( pClient->b4HisIP.b ),
                (int)pClient->iHisPort,
                (long)i32Time_ms,
                (long)pClient->dwNumBytesRcvd,
                (long)pClient->dwNumBytesSent );
        // Sometimes, a certain web browser simultaneously opened THREE connections
        // (sockets), but then only used ONE of them. See example in CwServer_OnConnect().
      }
     pClient->fDisconnect = TRUE;
   } // end if < "activity timeout" >


  if( iClientIndex != CWNET_LOCAL_CLIENT_INDEX ) // Anything left to send ?
   {
#   if( SWI_USE_HTTP_SERVER )  // support the optional, built-in HTTP server ?
     if( pClient->fUsesHTTP && (pClient->pHttpInst!=NULL) )
      { HttpSrv_OnPoll( pClient->pHttpInst );   // -> may fill pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ]
        return; // Don't enter the switch( ..iClientState ) futher below when HTTP is used !
      }
#   endif // SWI_USE_HTTP_SERVER ?

     switch( pClient->iClientState )
      { case CWNET_CLIENT_STATE_ACCEPTED  : // (1) accepted .. wait until this remote client identifies himself
           break;
        case CWNET_CLIENT_STATE_LOGIN_SENT: // (2) log-in "sent" : meaningless for the server side.
           break;
        case CWNET_CLIENT_STATE_LOGIN_CFMD: // (3) log-in CONFIRMED (by this server) ..
           // -> THIS is the normal state in which audio and Morse code is sent,
           //    or other UNSOLICITED data like reports of the VFO frequency,
           //    operation mode, and more:
           if( pClient->iPingTxCountdown_ms >= 0 )
            {  pClient->iPingTxCountdown_ms -= CWNET_SOCKET_POLLING_INTERVAL_MS;
            }
           if( pClient->iPingTxCountdown_ms < 0 ) // time to SEND another "PING" to this client ?
            { if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_PING, CWNET_PAYLOAD_SIZE_PING) ) != NULL )
               { pClient->iPingTxCountdown_ms = 2000; // wait at least 2 seconds until the next ping
                 // On this occasion, let the peer know our "local high-resolution timer value",
                 // scaled into MILLISECONDS, as 32-bit int but ranging from 0 .. 0x7FFFFFFF :
                 pClient->i32PingTimestamp_ms[0] = TIM_ReadHighResTimer_ms() & 0x7FFFFFFF;
                 pClient->i32PingTimestamp_ms[1] = pClient->i32PingTimestamp_ms[2] = 0;
                 pbPayload[0] = 0; // 0 = Ping-REQUEST (1 or 2 would be a Ping-RESPONSES)
                 pbPayload[1] = (BYTE)iClientIndex; // arbitrary "identifier" (here: the server's internal "client index")
                 pbPayload[2] = 0; // future reserved / alignment dummy ..
                 pbPayload[3] = 0;
                 memcpy( pbPayload+4, pClient->i32PingTimestamp_ms, 3*4); // keep it simple ... little endian (low byte first) ... BASTA !
                 TIM_StartStopwatch( &pClient->swPing );
                 if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                  &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
                       ||(pClient->nPeriodicMessagesInTrafficLog < 10 ) )                      // .. except for the first few messages
                   )    
                  { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d Ping[%d] t1=%ld ms, t2=%ld ms, t3=%ld ms",
                         (int)iClientIndex, (int)pbPayload[0],
                         (long)CwNet_ReadNonAlignedInt32( pbPayload+4 ),
                         (long)CwNet_ReadNonAlignedInt32( pbPayload+8 ),
                         (long)CwNet_ReadNonAlignedInt32( pbPayload+12) );
                    if(  pClient->nPeriodicMessagesInTrafficLog < 32767 )
                     { ++pClient->nPeriodicMessagesInTrafficLog;
                     }
                   }
               }
            } // end if < time to send another "PING" ? >

           // Need to report the VFO frequency, "S-meter", and maybe another parameter from server to this remote client ?
           //    For robustness and to ease debugging, send the following packet
           //    every 10 seconds even without a change in frequency,
           //    because CWNET_CMD_FREQ_REPORT also sends parameters that are not checked above.
           //    On the other hand, don't send "too frequently" to conserve bandwidth.
           i32Time_ms = TIM_ReadStopwatch_ms(&pClient->sw_VfoReport);
           if( ( (i32Time_ms>=100) || (i32Time_ms==0) ) // limit to 10 reports per second, or send if the timer doesn't run yet.
             &&( pCwNet->RigControl.dblVfoFrequency != RIGCTRL_NOVALUE_DOUBLE )  // only send a CWNET_CMD_FREQ_REPORT if we know AT LEAST the VFO frequency
             )
            { T_RigCtrl_VfoReport sVfoReport;
              DWORD dwHash;
              RigCtrl_AssembleVfoReport( &pCwNet->RigControl/*in*/, &sVfoReport/*out*/ );
              dwHash = UTL_CRC32( 0/*seed*/, (const BYTE*)&sVfoReport, sizeof(sVfoReport) );
              if( (dwHash != pClient->dwHashFromLastVfoReport) || (i32Time_ms >= 10000) )
               { if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_FREQ_REPORT, sizeof(T_RigCtrl_VfoReport) ) ) != NULL )
                  { memcpy( pbPayload, (BYTE*)&sVfoReport, sizeof(T_RigCtrl_VfoReport) );
                    TIM_StartStopwatch( &pClient->sw_VfoReport );
                    pClient->dwHashFromLastVfoReport = dwHash;
                    if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                     &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
                          ||(pClient->nPeriodicMessagesInTrafficLog < 20 ) ) // .. except for the first few messages
                      )
                     { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d FreqRpt f=%.1lf kHz, txStat=%d, S-meter=%d dB",
                         (int)iClientIndex,
                         (double)(sVfoReport.dblVfoFrequency*1e-3),
                         (int)sVfoReport.bTrxStatus, // +1 = transmitting, +2=audio muted, etc
                         (int)sVfoReport.i8SMeterLevel_dB );
                       if(  pClient->nPeriodicMessagesInTrafficLog < 32767 )
                        { ++pClient->nPeriodicMessagesInTrafficLog;
                        }
                     } // end if < show the CWNET_CMD_FREQ_REPORT in the traffic log > ?
                  }   // end if < enough space in the TX buffer > ?
               }     // // end if < need to report the VFO frequency, etc > ?
            }       // end if < enough time passed between two frequency reports > ?

           // Need to report some other parameter to send from the server (this end) to a remote client ?
           // (These parameters are identified by RigControl's "Unified" parameter number,
           //  e.g. RIGCTRL_PN_OP_MODE if the client operator has set a new
           //       OPERATION MODE in pCwNet->RigControl.iOpMode .
           //  Here, on the SERVER SIDE, queues of to-be-sent parameter numbers
           //        must be implemented for EACH REMOTE CLIENT.
           //  Thus, in contrast to the simpler CLIENT side (implemented further below),
           //        the queue filled by RigCtrl_AddParamToTxQueueForCwNet()
           //        is DUPLICATED for each of the server's potential remote clients
           //         - see CwNet_DuplicateParameterQueueForRemoteClients() .
           if( CwNet_GetFreeSpaceInTxBuffer(pCwNet) > (2*sizeof(T_RigCtrl_ParamReport_Double)) )
            { int iUnifiedPN;
              if( (iUnifiedPN=CwNet_GetParamNrFromTxQueueForClient(&pCwNet->Client[iClientIndex])) > RIGCTRL_PN_UNKNOWN ) // Here: SERVER side, with an extra queue for each REMOTE CLIENT.
               { // First get all we need to know about the to-be-sent parameter, e.g. data type:
                 T_RigCtrl_ParamInfo *pPI = RigCtrl_GetInfoForUnifiedParameterNumber( iUnifiedPN );
                 if( pPI != NULL ) // got a valid T_RigCtrl_ParamInfo for the to-be-sent parameter,
                  { // so try to allocate a block in the transmit buffer, just large enough
                    // for the data type, and send it from client to server:
                    CwNet_SendUnifiedParameterReport(pCwNet, iClientIndex, pPI);
                  }
               }
            } // end if < enough space in the TX buffer to send a 'parameter report' (with unified parameter number and value) ?

           // Need to report various values for the "Multi-Function Meter" to this client ?
           //    Limit the update rate, similar as for other packet types:
           i32Time_ms = TIM_ReadStopwatch_ms(&pClient->sw_MultiFunctionMeterReport);
           if( ( ( i32Time_ms >= 100) || (i32Time_ms==0) ) // limit to 10 reports per second, or send if the timer doesn't run yet.
             &&( pCwNet->RigControl.iParameterPollingState == RIGCTRL_POLLSTATE_DONE ) // don't report during initialisation phase
             )           
            { T_RigCtrl_MultiFunctionMeterReport sMeterReport;
              DWORD dwHash;
              RigCtrl_AssembleMultiFunctionMeterReport( &pCwNet->RigControl/*in*/, &sMeterReport/*out*/ );
              dwHash = UTL_CRC32( 0/*seed*/, (const BYTE*)&sMeterReport, sizeof(sMeterReport) );
              if( (dwHash != pClient->dwHashFromLastMeterReport) || (i32Time_ms >= 10000) )
               { if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_MULTI_FUNCTION_METER_REPORT, sizeof(sMeterReport) ) ) != NULL )
                  { memcpy( pbPayload, (BYTE*)&sMeterReport, sizeof(sMeterReport) );
                    TIM_StartStopwatch( &pClient->sw_MultiFunctionMeterReport );
                    pClient->dwHashFromLastMeterReport = dwHash;
                    if(   (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                     &&((!(pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_REJECT_PERIODIC_MSGS)) // .. thus the option to reject this..
                          ||(pClient->nMeterReportsInTrafficLog < 10 ) ) // .. except for the first few messages
                      )
                     {
                       RigCtrl_MultiFunctionMeterReportToString( &sMeterReport, sz255, 80 );
                       ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d MeterRp %s", (int)iClientIndex, sz255 );
                       if(  pClient->nMeterReportsInTrafficLog < 32767 )
                        { ++pClient->nMeterReportsInTrafficLog;
                        }
                     } // end if < show the CWNET_CMD_MULTI_FUNCTION_METER_REPORT in the traffic log > ?
                  }   // end if < enough space in the TX buffer to append a 'multi-function meter report' ?
               }     // end if < need to report data for the "Multi-Function Meter" display > ?
            }       // end if < enough time passed between two "Mult-Function Meter" reports > ?

           // Similar as the 'Multi-Funktion Meter' report, send a 'Poti' report (less frequently) ?
           i32Time_ms = TIM_ReadStopwatch_ms(&pClient->sw_PotiReport);
           if( ( ( i32Time_ms >= 500) || (i32Time_ms==0) ) // limit to 2 reports per second, or send if the timer doesn't run yet.
             &&( pCwNet->RigControl.iParameterPollingState == RIGCTRL_POLLSTATE_DONE ) // don't report during initialisation phase
             )
            { T_RigCtrl_PotiReport sPotiReport;
              DWORD dwHash;
              RigCtrl_AssemblePotiReport( &pCwNet->RigControl/*in*/, &sPotiReport/*out*/ );
              dwHash = UTL_CRC32( 0/*seed*/, (const BYTE*)&sPotiReport, sizeof(sPotiReport) );
              if( dwHash != pClient->dwHashFromLastPotiReport )
               { if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_POTI_REPORT, sizeof(sPotiReport) ) ) != NULL )
                  { memcpy( pbPayload, (BYTE*)&sPotiReport, sizeof(sPotiReport) );
                    TIM_StartStopwatch( &pClient->sw_PotiReport );
                    pClient->dwHashFromLastPotiReport = dwHash;
                    if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // occasionally sent, no "CPU hog"..
                     {
                       RigCtrl_PotiReportToString( &sPotiReport, sz255, 127 );
                       ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP,
                            "TX%d PotiRpt %s", (int)iClientIndex, sz255 );
                     } // end if < show the 'poti setting' report in the traffic log > ?
                  }   // end if < enough space in the TX buffer to append a 'poti setting report' ?
               }     // end if < need to report data for the "Poti Setting" display > ?
            }       // end if < enough time passed between two "Poti Setting" reports > ?

           if( pClient->iAnnouncedTxClient != pCwNet->iTransmittingClient )
            { // Time to inform this remote client about "who has the key" now !
              pClient->iAnnouncedTxClient = pCwNet->iTransmittingClient;
              pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX].iAnnouncedTxClient = pCwNet->iTransmittingClient;
              CwNet_SendTxAnnouncement( pCwNet, // <- sends a CWNET_CMD_TX_INFO ..
                CwNet_GetClientCallOrInfo(pCwNet,pClient->iAnnouncedTxClient));
            }
           iLen = CwNet_GetFreeSpaceInTxBuffer(pCwNet); // <- number of BYTES ..
           if( (pCwNet->pCwDSP != NULL) && (iLen>256)  // ok to "stream audio OUT" now ?
             &&(pCwNet->pCwDSP->cfg.iAudioFlags & DSP_AUDIO_FLAGS_ALLOW_NETWORK_AUDIO)
             )
            { // On the SERVER SIDE, T_CwDSP.sInputFifo is usually filled with
              // the radio-receiver's digitized, and DOWNSAMPLED audio signal
              // with 8 kSamples per second (in sInputFifo still in floating
              // point format). The additional A-Law compression further reduces
              // the required 'streaming bandwidth' to ~~ 8 kByte / second.
              nSamples = CwDSP_GetNumSamplesInFifoForTailIndex( &pCwNet->pCwDSP->sInputFifo, pClient->iS2CAudioTail );
              iLen2 = nSamples + 16/*spare bytes for framing*/;
              if( iLen > iLen2 )
               {  iLen = iLen2;
                  nSamples = iLen - 16;
               }
              if( nSamples >= 64 ) // avoid sending VERY short audio fragments (64 samples / 8 kHz = 8 ms)
               { if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_AUDIO, nSamples/*payload size in bytes*/) ) != NULL )
                  { // ok, allocated space in the network buffer so read samples and compress into ONE BYTE per sample:
                    for( i=0; i<nSamples; ++i )
                     { CwDSP_ReadFromFifo( &pCwNet->pCwDSP->sInputFifo, &pClient->iS2CAudioTail, &fltSample, 1/*nSamples*/, NULL/*pdblTimestamp_s*/ );
                       if( pCwNet->pCwDSP->cfg.iAudioFlags & DSP_AUDIO_FLAGS_SEND_NETWORK_TEST_TONE )
                        { pClient->iTestTonePhaseAccu += (1000/*Hz*/ * SOUND_COS_TABLE_LEN + (CWDSP_INPUT_FIFO_SAMPLING_RATE/2) )
                                                         / CWDSP_INPUT_FIFO_SAMPLING_RATE;
                          pClient->iTestTonePhaseAccu &= (SOUND_COS_TABLE_LEN-1);
                          fltSample += SoundTab_fltCosTable[ pClient->iTestTonePhaseAccu ] * 0.1; // -20 dB
                        }
                       CwDSP_FloatToShort( &fltSample/*pfltIn*/, &i16Sample/*pi16Out*/, 1/*nSamples*/, 32767.0/*factor*/ );
                       *pbPayload++ = LinearToALawSample(i16Sample);
                     }
                    pCwNet->nAudioSamplesSent_Mod1k += nSamples; // count SENT audio sample, for diagnostics ..
                    // (At fSample=8 kHz, a 32-bit sample counter would overflow
                    //  (become negative) after (2^31 / 8000Hz) = 3 days. Not good.
                    //  Solution: Accumulate the count in an integer up to 1000 samples,
                    //            and when reaching or exceeding 1000 samples there,
                    //            'carry' to the 32-bit counter below,
                    //            which will run for YEARS before flowing over.
                    // (Microcontroller-friendly trick, borrowed from a firmware project)
                    while( pCwNet->nAudioSamplesSent_Mod1k >= 1000 )
                     { ++pCwNet->i32AudioSamplesSent_k;
                       pCwNet->nAudioSamplesSent_Mod1k -= 1000;
                     }
                  }
               }   // end if < enough samples available to send (from Client to Server) > ?
            }     // end if < ok to stream audio now > ?

#         if( SWI_NUM_AUX_COM_PORTS > 0 )
           // On a similar 'priority level' as sending audio samples (above):
           //  If the space in the TX-buffer still permits at this point,
           //  send another chunk bytes from one of the 'Additional COM Ports',
           //  when configured as "Client/Server Tunnel" (that's the term used
           //  in the manual, see Remote_CW_Keyer.htm#Additional_COM_Ports) ?
           CwNet_PollForTransmissionFromAuxComPort(pCwNet,pClient);
#         endif // SWI_NUM_AUX_COM_PORTS > 0 ?

           // Need to report a new 'spectrum reference level' to this remote client ?
           if( (pCwNet->RigControl.dblScopeRefLevel_dB != pClient->dblScopeRefLevel_dB )
             &&(pCwNet->RigControl.dblScopeRefLevel_dB != RIGCTRL_NOVALUE_DOUBLE) ) // don't report something we don't know
            { // (Icom's "scope REF level" is not periodically sent in our T_RigCtrl_Spectrum,
              //  thus the need to send it IN AN EXTRA 'report', like a few other parameters)
              if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_PARAM_DOUBLE, sizeof(T_RigCtrl_ParamReport_Double) ) ) != NULL )
               { T_RigCtrl_ParamReport_Double p;
                 memset( &p, 0, sizeof(T_RigCtrl_ParamReport_Double) );
                 p.bStructSize = sizeof(T_RigCtrl_ParamReport_Double); // struct size in bytes is PART OF THE STRUCT
                 p.bRigControlParameterNumber = RIGCTRL_PN_SCOPE_REF_LEVEL;
                 p.dblParameterValue = pClient->dblScopeRefLevel_dB = pCwNet->RigControl.dblScopeRefLevel_dB;
                 memcpy( pbPayload, (BYTE*)&p, sizeof(T_RigCtrl_ParamReport_Double) );
               }
            } // end if < need to report a new 'spectrum reference level' > ?

           // Need to report a new "Scope Span" (width of the spectrum display in Hertz) from server to this remote client ?
           if( (pCwNet->RigControl.dblScopeSpan_Hz != pClient->dblScopeSpan_Hz )
             &&(pCwNet->RigControl.dblScopeSpan_Hz != RIGCTRL_NOVALUE_DOUBLE) ) // don't report something we don't know
            { if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_PARAM_DOUBLE, sizeof(T_RigCtrl_ParamReport_Double) ) ) != NULL )
               { T_RigCtrl_ParamReport_Double p;
                 memset( &p, 0, sizeof(T_RigCtrl_ParamReport_Double) );
                 p.bStructSize = sizeof(T_RigCtrl_ParamReport_Double); // struct size in bytes is PART OF THE STRUCT
                 p.bRigControlParameterNumber = RIGCTRL_PN_SCOPE_SPAN;
                 p.dblParameterValue = pClient->dblScopeSpan_Hz = pCwNet->RigControl.dblScopeSpan_Hz;
                 memcpy( pbPayload, (BYTE*)&p, sizeof(T_RigCtrl_ParamReport_Double) );
               }
            } // end if < need to report a new 'Scope Span' setting > ?

           // If there is still enough space in the outbound network buffer,
           // also send what Icom called "waveform data" (actually SPECTRA,
           // already parsed from CI-V and placed in a FIFO by RigControl.c) ?
           iLen = CwNet_GetFreeSpaceInTxBuffer(pCwNet); // <- number of BYTES ..
           // "Waveform data" (spectra) are pulled from the same tiny FIFO
           // that also feeds the 'local' spectrum / waterfall display,
           // as in SpecDisp.cpp : SpecDisp_UpdateWaterfall() :
           nSamples = pCwNet->RigControl.nSpectrumBinsUsed;
           nSpectra = RigCtrl_GetNumSpectraAvailableInFIFO( &pCwNet->RigControl,
                               pClient->iSpectrumBufferTail );
           if( (nSamples>0) // RigControl.c seems to deliver spectra ..
             &&(nSpectra>0) // ... and there are spectra available that haven't been sent yet ..
             &&(iLen>(nSamples+(int)sizeof(T_CwNet_SpectrumHeader)+64)) // .. and there's enough space for transmission
             )
            { // There's at least one more spectrum waiting for transmission
              // to THIS REMOTE CLIENT (pClient). "Waveform data" (spectra)
              // are pulled from the same FIFO (in RigControl.c) that also
              // also feeds the 'local' spectrum / waterfall display,
              //   for example in SpecDisp.cpp : SpecDisp_UpdateWaterfall() :
              T_RigCtrl_Spectrum *pSpectrum = RigCtrl_GetSpectrumFromFIFO(
                         &pCwNet->RigControl, &pClient->iSpectrumBufferTail );
              // When read from an IC-7300 via USB, despite the "Waterfall Speed"
              // set to HIGH, only a few spectra arrived per second,
              // so with only 475 frequency bins per spectrum, there's no need
              // for a fancy 'waveform data compression' (unlike e.g. KiwiRX).
              // Just send ONE BYTE (signed, ranging from e.g. -127 to +127 dB
              // "over" the phantasy reference used by the radio) per bin,
              // along with the bare minimum required to display them:
              if( (pSpectrum != NULL)
              && ((pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_SPECTRUM,
                    nSamples+sizeof(T_CwNet_SpectrumHeader)/*payload size*/) ) != NULL ) )
               { // ok, allocated space in the network buffer so convert another spectrum:
                 T_CwNet_SpectrumHeader spectrumHeader;
                 // Fill out the T_CwNet_SpectrumHeader as a local variable,
                 // because the pbPayload (byte pointer) may not be properly aligned
                 // for 32 bit !
                 spectrumHeader.u16NumBinsUsed = (unsigned short)nSamples;
                 spectrumHeader.u16Reserved    = 0;
                 spectrumHeader.flt32BinWidth_Hz = pSpectrum->dblBinWidth_Hz;
                 spectrumHeader.flt32FreqMin_Hz  = pSpectrum->dblFmin_Hz;
                 memcpy(pbPayload, &spectrumHeader, sizeof(T_CwNet_SpectrumHeader) );
                 pbPayload += sizeof(T_CwNet_SpectrumHeader);
                 for( i=0; i<nSamples; ++i )
                  { fltSample = pSpectrum->fltMagnitudes_dB[i];
                    if( fltSample >= 127.0 ) // limit before converting to 8-bit signed integer
                     {  fltSample  = 127.0;
                     }
                    if( fltSample <= -127.0 )
                     {  fltSample  = -127.0;
                     }
                    pbPayload[i] = (BYTE)( (signed char)fltSample );
                  }  // end for < all 'frequency bins' in the spectrum >
               }    // end if < sufficiently allocates space in the TX buffer >
            }      // end if < send another SPECTRUM (Icom's "Waveform data") ?

           // Send another entry in the 'Band Stacking Registers' to this remote client ?
           if( pClient->iBandStackingRegsTxIndex >= 0 )  // <- array index and "counter for transmission", started at zero in ServerThread() when 'RigControl' signalled "something has changed in pCwNet->RigControl.BandStackingRegs[]"
            { iLen = CwNet_GetFreeSpaceInTxBuffer(pCwNet); // <- number of BYTES ..
              // Using RigCtrl_FreqMemEntryToString(), a typical entry in
              // T_RigCtrlInstance.BandStackingRegs[] will be converted into an
              // easily parsable (not "parseable") string of key=value pairs like this:
              // "i=0 fr=1915000.0 mo=CWNN" (band stacking entries don't have a MEMORY NAME, other "memory channels" do)
              while( (iLen > 80 ) // don't waste time if there's insufficient space in the TX buffer..
                && ((i=pClient->iBandStackingRegsTxIndex) < pCwNet->RigControl.iNumBandStackingRegs)
                && (i>=0) )
               { iLen2 = RigCtrl_FreqMemEntryToString(
                            &pCwNet->RigControl.BandStackingRegs[i], // [in] T_RigCtrlFreqMemEntry
                            i,  // [in] zero-based array index for easy parsing (or negative=no array)
                            pCwNet->RigControl.iNumBandStackingRegs, // [in] NUMBER of entries (service for the receiver to detect when all items are through)
                            sz255, 250/*iMaxDestLen*/ );  // [out] key=value pairs, comma separated
                 if( (iLen2>0) && ((iLen2+1) < iLen ) )
                  { ++iLen2; // Add one byte to include the trailing zero in the transmitted data.
                             // This simplifies parsing on the receiver's side later, as a C-string.
                    pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_BAND_STACKING_REGISTER, iLen2 );
                    if( pbPayload != NULL ) // allocation successful ?
                     { memcpy(pbPayload, sz255, iLen2);
                       ++i;
                       if( i < pCwNet->RigControl.iNumBandStackingRegs ) // more entries to send ?
                        { pClient->iBandStackingRegsTxIndex = i;
                        }
                       else // bingo, finished "sending" all BAND STACKING REGISTERS to this client !
                        { pClient->iBandStackingRegsTxIndex = -1;
                        }
                       if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                        { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d BandStk [%04X] %s",
                                   (int)iClientIndex, (int)(iLen2-1), sz255 ); // format as for the "RX" side
                        }
                     }
                  }
                 else // the string is too long for the "network buffer", so..
                  { break; // stop sending the list in this over, continue later
                  }
               } // end while < sufficient space in the network buffer to send more 'Band Stacking Registers' >
            } // end if( pClient->iBandStackingRegsTxIndex >= 0 )
           break; // case CWNET_CLIENT_STATE_LOGIN_CFMD [here on the SERVER side]
        case CWNET_CLIENT_STATE_DISCONN  : // disconnected .. ignore this remote client
        default:
           break;

      } // end switch( pClient->iClientState ) for the SERVER implementation
   }
  else // not 'server' but 'client side' -> Does THIS CLIENT have anything 'unsolicited' to send ?
   { switch( pClient->iClientState )
      { case CWNET_CLIENT_STATE_CONNECTED: // (1) this LOCAL CLIENT shall identify himself
           // by sending this 'unsolicited' command (struct T_CwNet_ConnectData).
           // The user name is mandatory, the callsign may be empty in "SWL mode" .
           if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_CONNECT, sizeof(T_CwNet_ConnectData) ) ) != NULL )
            { T_CwNet_ConnectData connData;
              memset( &connData, 0, sizeof(connData) );
              SL_strncpy( connData.sz40UserName, pCwNet->cfg.sz80ClientUserName, 40 );
              SL_strncpy( connData.sz40Callsign, pCwNet->cfg.sz80ClientCallsign, 40 );
              connData.dwPermissions = 0;  // just for completeness.. permissions are assigned by the SERVER, not the client !
              memcpy( pbPayload/*dest*/, &connData/*source*/, sizeof(connData)/*nBytes*/ );
              pClient->iClientState = CWNET_CLIENT_STATE_LOGIN_SENT; // .. hopefully soon..
              // if the remote server accepts our USER NAME,
              //  the next transition happens in CwNet_ExecuteCmd() .
              if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
               { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d Conn user=\"%s\" call=\"%s\"",
                               (int)iClientIndex, connData.sz40UserName, connData.sz40Callsign );
               }
            }
           break;
        case CWNET_CLIENT_STATE_LOGIN_SENT: // (2) wait patiently for a log-in response.
           // If we're not connected to a REMOTE CW SERVER but e.g. an HTTP server,
           // this client will simply time-out and then say goodbye.
           break;
        case CWNET_CLIENT_STATE_LOGIN_CFMD: // (3) log-in CONFIRMED (by remote server):
           // only in THIS state this client may send 'NORMAL unsolicited data' !
           // Send timestamped ON / OFF keying events from the 'Morse Output FIFO' ?
           iHeadIndex = (int)pCwNet->MorseTxFifo.iHeadIndex & (CW_KEYING_FIFO_SIZE-1);
           iTailIndex = (int)pCwNet->MorseTxFifo.iTailIndex & (CW_KEYING_FIFO_SIZE-1);
           // In this "classic", lock-free circular FIFO,
           //    HeadIndex == TailIndex means the FIFO is *empty*, thus:
           iLen = iHeadIndex - iTailIndex;
           if(iLen < 0 )  // circular FIFO index wrapped around ?
            { iLen += CW_KEYING_FIFO_SIZE;
            }
           if(iLen > 0 ) // at least one "key-up" or "key-down" event waiting for transmission ?
            { if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_MORSE, iLen) ) != NULL )
               { // Allocation in the "outbound network buffer" successful -> send all we have
                 while( (iLen--) > 0 )
                  { *pbPayload++ = pCwNet->MorseTxFifo.elem[ iTailIndex ].bCmd;
                    iTailIndex = (iTailIndex+1) & (CW_KEYING_FIFO_SIZE-1);
                  }
                 pCwNet->MorseTxFifo.iTailIndex = iTailIndex;
               }
            } // end if < Morse-key-"up" and/or "down"-events waiting in the FIFO for transmission ?

           // Request to "QSY" from this client to the remote server ?
           if( (pCwNet->RigControl.dblVfoFrequency != pClient->dblReportedVfoFrequency )
             &&(pCwNet->RigControl.dblVfoFrequency != RIGCTRL_NOVALUE_DOUBLE ) )
            { if( (pbPayload = CwNet_AllocBytesInTxBuffer( pCwNet, CWNET_CMD_FREQ_REPORT, sizeof(T_RigCtrl_VfoReport) ) ) != NULL )
               { T_RigCtrl_VfoReport sVfoReport;
                 RigCtrl_AssembleVfoReport( &pCwNet->RigControl, &sVfoReport );
                 memcpy( pbPayload, (BYTE*)&sVfoReport, sizeof(T_RigCtrl_VfoReport) );
                 pClient->dblReportedVfoFrequency = pCwNet->RigControl.dblVfoFrequency;
                 if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                  { ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d SetFreq: %.1lf kHz",
                               (int)iClientIndex, (double)pClient->dblReportedVfoFrequency );
                  }
               }
            } // end if < request "QSY" from this client > ?

           // Send a new "PTT" state (here: communicate the 'local PTT flag'
           //           from this client to the remote server) ?
           if( (pCwNet->RigControl.iTransmitReqst != pClient->iSentTransmitReqst )
            && (pCwNet->RigControl.iTransmitReqst != RIGCTRL_NOVALUE_INT ) )
            { // Since this doesn't happen very frequently, use the 'rigctld'-compatible,
              // debugger friendly syntax as an ASCII string ("set_ptt 0" or "set_ptt 1"):
              sprintf( sz255, "set_ptt %d\n", (int)pCwNet->RigControl.iTransmitReqst );
              if( CwNet_SendRigctldCompatibleString( pCwNet, pClient, sz255 ) )
               { pClient->iSentTransmitReqst = pCwNet->RigControl.iTransmitReqst;
               }
              // On the SERVER SIDE, CwNet_ExecuteCmd() -> CwNet_ExecuteRigctldCmd()
              //    will do the rest, and (if this client's permissions allow)
              //    switch the REAL PTT driving the radio. The server will also
              //    inform all other clients about the new PTT state.
            } // end if < send new value of pCwNet->RigControl.iTransmitReqst from this client > ?

           // Any other parameter to send from this client(!) to the remote server(!) ?
           // (identified by RigControl's "Unified" parameter number,
           //  e.g. RIGCTRL_PN_OP_MODE if the client operator has set a new
           //       OPERATION MODE in pCwNet->RigControl.iOpMode, using a
           //       thread-safe QUEUE of parameter numbers waiting to be sent)
           //       That queue is filled via RigCtrl_AddParamToTxQueueForCwNet(),
           //       called from the dozens of 'Set'-functions in RigControl.c,
           //       e.g. RigCtrl_SetOperatingMode(), etc etc etc etc,
           //       if the to-be-set parameter was MODIFIED since the last call.
           if( CwNet_GetFreeSpaceInTxBuffer(pCwNet) > (2*sizeof(T_RigCtrl_ParamReport_Double)) )
            { int iUnifiedPN;
              if( (iUnifiedPN=RigCtrl_GetParamNrFromTxQueueForCwNet(&pCwNet->RigControl)) > RIGCTRL_PN_UNKNOWN ) // Here: CLIENT side, with only ONE consumer for the queued-up parameter numbers
               { // First get all we need to know about the to-be-sent parameter, e.g. data type:
                 T_RigCtrl_ParamInfo *pPI = RigCtrl_GetInfoForUnifiedParameterNumber( iUnifiedPN );
                 if( pPI != NULL ) // got a valid T_RigCtrl_ParamInfo for the to-be-sent parameter,
                  { // so try to allocate a block in the transmit buffer, just large enough
                    // for the data type, and send it from client to server:
                    CwNet_SendUnifiedParameterReport(pCwNet, iClientIndex, pPI);
                  }
               }
            } // end if < enough space in the TX buffer to send a 'parameter report' (with unified parameter number and value) ?

#         if( SWI_NUM_AUX_COM_PORTS > 0 )
           // Also HERE (for the CLIENT side of a TCP/IP socket) :
           //  If the space in the TX-buffer still permits at this point,
           //  send another chunk bytes from one of the 'Additional COM Ports',
           //  when configured as "Client/Server Tunnel" (that's the term used
           //  in the manual, see Remote_CW_Keyer.htm#Additional_COM_Ports) ?
           CwNet_PollForTransmissionFromAuxComPort(pCwNet,pClient);
#         endif // SWI_NUM_AUX_COM_PORTS > 0 ?

           break; // end case CWNET_CLIENT_STATE_LOGIN_CFMD [here on the CLIENT side]
        case CWNET_CLIENT_STATE_DISCONN  : // disconnected .. ignore this remote client
        default:
           break;
      }
   } // end if < server or client side > ?

} // end CwNet_OnPoll()

//---------------------------------------------------------------------------
static void CwNet_OnReceive( T_CwNet *pCwNet, // used by BOTH, Client- *AND* Server-side (*)
              T_CwNetClient *pClient,         // [in] instance data for a CLIENT
              BYTE *pbRcvdData, int nBytesRcvd ) // [in] received stream segment
              // ,------------------'
              // '-> may be zero or even negative if a connection "broke down" .
  // [out, optional] pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ]
  //      = "anything to send", limited to
  //         CWNET_SOCKET_TX_BUFFER_SIZE - pCwNet->nBytesInTxBuffer (on entry)
  // (*) CwNet_OnReceive() contains the 'bytestream parser',
  //     used for ANYTHING received on any socket (in both directions).
  //
{
  BYTE b;
  char *pszInfo, sz40Info[44], sz1kDump[1024];
  // For certain commands (or bytestream types), we need the CLIENT INDEX,
  // for example to tell if the remote client (here, represented by pClient)
  // 'currently has the key' (may key the transmitter) :
  int iClientIndex = (int)(pClient - &pCwNet->Client[0]); // perfectly legal "pointer trick":
      // iClientIndex = 0..CWNET_MAX_CLIENTS-1 means pClients points into the
      //                above array (Client[]), and may be compared
      //                against pCwNet->iTransmittingClient further below.
  double dblTimestampForRcvdAudioSample_s = DSW_ReadHighResTimestamp_s();

  if( (pClient==NULL) || (pCwNet==NULL) || (pbRcvdData==NULL) )
   { return;
   }

  if( nBytesRcvd > 0 )
   { CwServer_FeedActivityWatchdog( pClient ); // feed the dog .. here: after RECEPTION
   }

  //  If this is the FIRST segment received from this client, check if the
  //  other guy isn't a WEB BROWSER (or some hacker thing trying to attack
  //  a web browser). For example, when 'visited' by Firefox on an Android
  //  smartphone to test the 'dynamic' DNS with port forwarding, using an
  //  URL like "http://MyServer.dynv6.net:7355/", CwNet_OnReceive() was called
  //  with pClient->dwNumBytesRcvd==0 and pClient->dwNumSegmentsRcvd==0,
  //  and the usual chatty string exposing more than the PHONE USER expects
  //  in pbRcvdData:
  //     "GET / HTTP/1.1\r\n"
  //     "Host: MyTestServer.dynv6.net:7355\r\n"
  //     "User-Agent: Mozilla/5.0 (Android 13; Mobile; rv:121.0) Gecko/121.0 Firefox/121.0\r\n"
  //     "Accept: text/html,application/xhtml+xml,application/xml,blah,blubb\r\n"
  //            ....
  //     "Connection: keep-alive\r\n"
  //     "Upgrade-Insecure-Requests: 1\r\n
  //     "\r\n"                              <- that's the EMPTY LINE = "end of the GET-request"
  //  To avoid having to check for all this stuff (that we don't support anyway),
  //  check in advance if the remote client is an "ordinary web browser"
  //  who tries to talk HTTP with us. If he does, DON'T LET ANYTHING ELSE FROM HIM
  //  pass to the 'bytestream parser' further below.
  pszInfo = "binary";
  if( (pClient->dwNumBytesRcvd==0) && (pClient->dwNumSegmentsRcvd==0) // very first call ?
    && (nBytesRcvd > 16) ) // Enough bytes to check for "GET"/"POST"/etc and the trailing "\r\n\r\n" ?
   { HERE_I_AM__CWNET();
     if( SL_strncmp( (char*)pbRcvdData + nBytesRcvd - 4, "\r\n\r\n", 4 ) == 0 ) // looks like HTTP ending with an empty LINE..
      { const char *cpSrc = (const char*)pbRcvdData;
        int iToken = SL_SkipOneOfNStrings( &cpSrc, HttpMethods ); // "GET", "POST" (to name just a few)
        if( iToken > 0 ) // any of the tokens listed in HttpMethods[] ?
         { pClient->fUsesHTTP = TRUE;  // -> pass everything on to HttpSrv_OnReceive() from now on
           if( ! (pCwNet->cfg.iHttpServerOptions & HTTP_SERVER_OPTIONS_ENABLE) )
            { sprintf( sz40Info, "%s,rejected", pszInfo );
              pszInfo = sz40Info;
            }
         }
      } // end if < may be HTTP because 'looks like text and ends with an EMPTY LINE' >
   } // end if < first reception on a new connection with what MAY BE a request from a WEB BROWSER >

  HERE_I_AM__CWNET();

  if( (pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_CONN_LOG)
    &&(pClient->dwNumSegmentsRcvd == 0)   // don't flooding the 'debug log' ..
    &&(!pClient->fUsesHTTP) )  // .. and only emit the log-entry here if it's not emitted in HttpServer.c
   { HERE_I_AM__CWNET();
     ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Server%d: connected %s %s:%d (%s)",
                //    ,------------------------------------------'   ,---------'  |  |...
                (int)iClientIndex, (const char*)((iClientIndex==0)?"to":"by"), // |  |
                     CwNet_IPv4AddressToString( pClient->b4HisIP.b ), // <--------'  |
                (int)pClient->iHisPort, pszInfo );                    // <-----------'
   }

  ++pClient->dwNumSegmentsRcvd;
  if( nBytesRcvd > 0 )
   { pClient->dwNumBytesRcvd += nBytesRcvd; // here: nunber of bytes PER CONNECTION (socket).
   }

#if(0) // Removed 2025-01, because with all those AUDIO- and SPECTRUM data, too much for the 'Debug' tab !
  // Received traffic is selectively 'dumped to the log' in e.g. CwNet_ExecuteCmd()
  if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
   { HERE_I_AM__CWNET();
     INET_DumpTraffic_HexOrASCII( sz1kDump,500, pbRcvdData, nBytesRcvd );
     ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "RX%d %s",
                 (int)iClientIndex, sz1kDump );
   }
#endif // removed 2025-01

#if( SWI_USE_HTTP_SERVER )  // support the optional, built-in HTTP server ?
  HERE_I_AM__CWNET();
  if( pClient->fUsesHTTP )  // HTTP ? Not THIS MODULE's business ..
   { if( pCwNet->cfg.iHttpServerOptions & HTTP_SERVER_OPTIONS_ENABLE ) // and only if we ALLOW HTTP at all ...
      { HERE_I_AM__CWNET();
        HttpSrv_OnReceive( pCwNet, pClient, pbRcvdData, nBytesRcvd );
        HERE_I_AM__CWNET();
        return;
      }
     else // looks like HTTP ( "GET", "POST", or whatever request),
      {   // but the built-in HTTP server is not enabled at all ->
        pClient->fDisconnect = TRUE; // drop the connection without sending any response (not even a "404"-error)
      }
   } // end if( pClient->fUsesHTTP )
#endif // SWI_USE_HTTP_SERVER ?

  // Pass everything through the simplistic 'received bytestream parser':
  HERE_I_AM__CWNET();
  while( (nBytesRcvd--) > 0 )
   { b = *pbRcvdData++;
     switch( pClient->iRxParserState )
      { case CWNET_PSTATE_RX_CMD : // "this byte should be a COMMAND" ..
           pClient->iCurrentCommand = b & CWNET_CMD_MASK_COMMAND;
           pClient->iCmdDataLength  = 0;
           pClient->iCmdDataByteIndex = 0;
           switch( b & CWNET_CMD_MASK_BLOCKLEN ) // do we expect a BLOCK LENGTH after the command ?
            { case CWNET_CMD_MASK_NO_BLOCK:    // "no BLOCK LENGTH INDICATOR after the command byte"
                 pClient->iRxParserState = CWNET_PSTATE_RX_CMD; // immediately receive the next COMMAND BYTE
                 pClient->bCmdDataBuffer[0] = '\0'; // trailing zero, for what it's worth
                 CwNet_ExecuteCmd( pCwNet, pClient );
                 break;
              case CWNET_CMD_MASK_SHORT_BLOCK: // "short block" (ONE-BYTE length, i.e. max 255 byte payload)
                 pClient->iRxParserState = CWNET_PSTATE_RX_SHORT_LEN;
                 break;
              case CWNET_CMD_MASK_LONG_BLOCK : // "long block" (TWO-BYTE length, i.e. max 65535 byte payload)
                 pClient->iRxParserState = CWNET_PSTATE_RX_LENGTH_LO;
                 break;
              default: // case CWNET_CMD_MASK_RESERVE :
                 pClient->iRxParserState = CWNET_PSTATE_RX_CMD; // immediately receive the next COMMAND BYTE
                 break;
            } // end switch( b & CWNET_CMD_MASK_BLOCKLEN )
           break; // end case CWNET_PS_RX_CMD
        case CWNET_PSTATE_RX_SHORT_LEN: // "this byte is a SINGLE-BYTE block length"
           pClient->iCmdDataLength = b;
           if( pClient->iCmdDataLength <= 0 )
            { // hmm.. command with a "short block" (8-bit "length" field),
              //       but the block is EMPTY !
              pClient->iRxParserState = CWNET_PSTATE_RX_CMD; // discard as garbage !
            }
           else
            { pClient->iRxParserState = CWNET_PSTATE_RX_BLOCK;
            }
           break; // end case CWNET_PS_RX_SHORT_LEN
        case CWNET_PSTATE_RX_LENGTH_LO: // "this byte is the LSB of a 16-bit BLOCK LENGTH"
           pClient->iCmdDataLength = b;
           pClient->iRxParserState = CWNET_PSTATE_RX_LENGTH_HI;
           break;
        case CWNET_PSTATE_RX_LENGTH_HI: // "this byte is the MSB of a 16-bit BLOCK LENGTH"
           pClient->iCmdDataLength |= ((WORD)b << 8);
           if( pClient->iCmdDataLength <= 0 )
            { // hmm.. command with a "long block" (16 bit length field),
              //       but the block is EMPTY !
              pClient->iRxParserState = CWNET_PSTATE_RX_CMD; // discard as garbage !
            }
           else
            { pClient->iRxParserState = CWNET_PSTATE_RX_BLOCK;
            }
           break; // end case CWNET_PS_RX_LENGTH_HI
        case CWNET_PSTATE_RX_BLOCK : // "this byte (b) belongs to the DATA BLOCK after a command".
           // Whether to STORE or "immediately process" it depends on the command:
           if( pClient->iCmdDataByteIndex < CWNET_CMD_DATA_BUFFER_SIZE )
            {  pClient->bCmdDataBuffer[pClient->iCmdDataByteIndex] = b;
               // For STRINGS (as after CWNET_CMD_CONNECT), this includes the
               // trailing zero, which turns pClient->bCmdDataBuffer[]
               // into an easy-to-process "C"-string. If there's no trailing
               // zero sent via the network, we add one immediately before
               // calling CwNet_ExecuteCmd().
            }
           pClient->iCmdDataByteIndex++; // <- this may exceed CWNET_CMD_DATA_BUFFER_SIZE,
           // which is important to enter pClient->iRxParserState = CWNET_PSTATE_RX_CMD further below !
           switch( pClient->iCurrentCommand )
            { case CWNET_CMD_MORSE : // this REMOTE OPERATOR sends Morse code (not text / ASCII) ...
                 // .. only pass this on to the two-millisecond 'keyer thread'
                 //    if pClient represents a REMOTE CLIENT that may key the transmitter:
                 if( (iClientIndex >= CWNET_FIRST_REMOTE_CLIENT_INDEX)
                  && (iClientIndex <= CWNET_MAX_CLIENTS)
                  && (pClient->iPermissions & CWNET_PERMISSION_TRANSMIT) )
                  { // Ok, we are the SERVER, the guy at the other end is a REMOTE CLIENT,
                    // and the server's sysop has granted him the permission to TRANSMIT.
                    // He MAY "take the key" if the user who previously 'had it'
                    // has obviously finished his 'over' :
                    if( pCwNet->iTransmittingClient<0 ) // no one "has the key", so...
                     { pCwNet->iTransmittingClient = iClientIndex; // .. hand it over..
                     }
                    pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX].iAnnouncedTxClient = pCwNet->iTransmittingClient; // <- for the GUI (server side)
                    if( pCwNet->iTransmittingClient==iClientIndex ) // "got the key" now ?
                     { int iFifoHeadIndex = pCwNet->MorseRxFifo.iHeadIndex & (CW_KEYING_FIFO_SIZE-1);
                       pCwNet->MorseRxFifo.elem[ iFifoHeadIndex ].bCmd = b; // <- food for KeyerThread() (*)
                       pCwNet->MorseRxFifo.elem[ iFifoHeadIndex ].i32TimeOfReception_ms = TIM_ReadHighResTimer_ms();
                       // ,---------------------------------------'
                       // '--> Used in Keyer_GetMorseOutputFromRxFifo() to decide
                       //      when to START, depending on the network latency .

                       pCwNet->MorseRxFifo.iHeadIndex = (iFifoHeadIndex+1) & (CW_KEYING_FIFO_SIZE-1);
                       // (*) The rest happens in KeyerThread(),
                       //       which drains pCwNet->MorseRxFifo[],
                       //       and actually 'keys the transmitter'.
                     } // end if < remote client HAS GOT the key >
                  } // end if < server side, and the remote client MAY transmit >
                 break; // end case CWNET_CMD_MORSE
              case CWNET_CMD_AUDIO : // the peer (or the server's radio) sends *audio* ..
                 // .. since we don't support remote control / voice transmissions
                 //    like WVVIEW (yet?), just discard it ... or 'mix' it
                 //    to the audio received from the radio, and pass it to
                 //    ALL OTHER CLIENTS currently connected to this server ?
                 if( pCwNet->pCwDSP != NULL )  // ok to "stream audio IN" now ?
                  { float fltSample = (float)ALawDecompressTable[ b ] / 32767.0; // 8 -> 16 bits per sample, A-Law decompression
                    // Let the audio-DSP add the received stream add to the
                    // output for the speaker (if this instance runs as CLIENT),
                    // or use the sample to 'modulate the transmitter' (if this
                    // instance runs as SERVER, locally connected to a radio) :
                    CwDSP_ProcessSampleFromReceivedAudioStream( pCwNet->pCwDSP, fltSample, dblTimestampForRcvdAudioSample_s );
                    dblTimestampForRcvdAudioSample_s += (1.0 / (double)CWDSP_NETWORK_FIFO_SAMPLING_RATE);
                    // Count RECEIVED audio samples (in units of 1000 samples, to avoid early 32-bit overflow):
                    ++pCwNet->nAudioSamplesRcvd_Mod1k; // count another RECEIVED audio sample, for diagnostics ..
                    if( pCwNet->nAudioSamplesRcvd_Mod1k >= 1000 )
                     {  pCwNet->nAudioSamplesRcvd_Mod1k -= 1000;
                      ++pCwNet->i32AudioSamplesRcvd_k; // <- shown in the "Test Report" on the GUI's debug tab
                     }
                  }
                 break; // end case CWNET_CMD_AUDIO [here: stream audio IN]
              case CWNET_CMD_CI_V :   // this REMOTE OPERATOR sends a CI-V command ..
              case CWNET_CMD_CONNECT: // "Connect" or "Connected" ...
              case CWNET_CMD_SPECTRUM:
              case CWNET_CMD_PING :   // "Ping"  : also not parsed on-the-fly, but on COMPLETION, in CwNet_ExecuteCmd()
              case CWNET_CMD_PRINT:   // "Print" : also not parsed on-the-fly, but on COMPLETION, ...
                 // For all the above commands, simply store the received byte(s)
                 //   in pClient->bCmdDataBuffer[CWNET_CMD_DATA_BUFFER_SIZE],
                 //   and don't care for them until the reception of the block is complete.
                 //   The rest happens in CwNet_ExecuteCmd(), further below.
                 // For commands with LONGER BLOCKS, e.g. CWNET_CMD_AUDIO,
                 //   etc(?), this won't work !
                 break;

              case CWNET_CMD_RIGCTLD : // 0x06 : 'rigctld'-compatible ASCII string (plus a trailing ZERO) :
                 // Not parsed/executed HERE (immediately, as the characters pour in one-by-one)
                 // but in CwNet_ExecuteCmd() !
                 break;

#            if( SWI_NUM_AUX_COM_PORTS > 0 )
              case CWNET_CMD_SERIAL_PORT_TUNNEL_ANY: // .. see AuxComPorts.c, mode RIGCTRL_PORT_USAGE_SERIAL_TUNNEL
              case CWNET_CMD_SERIAL_PORT_TUNNEL_1: // we support up to THREE tunnels over TCP/IP,
              case CWNET_CMD_SERIAL_PORT_TUNNEL_2: //    regardless of SWI_NUM_AUX_COM_PORTS,
              case CWNET_CMD_SERIAL_PORT_TUNNEL_3: // plus the above tunnel-index for "ANY" tunnel.
                 pClient->iCurrentCommand = pClient->iCurrentCommand; // place for a breakpoint,
                    // because C++ builder didn't allow one on the call of
                    //  AuxCom_OnReceptionFromNetworkPortTunnel() further below.
                    // On the above DUMMY, it was still impossible to set a breakpoint,
                    // but on the NEXT executable instruction, a breakpoint was ok. Aaargh !
                 //
                 // For this command ('Serial Port Tunnel'), simply pass on the received byte
                 // to module "AuxComPorts.c". AuxComThread() will do the rest,
                 // and SEND the contents of T_AuxComPortFifo.sRxFifo to one
                 // or more local 'COM' ports (serial ports, usually connected to
                 // some external applications via a 'virtual null-modem cable'.
                 // This is stoneage technology, but suitable to be implemented on the
                 // tiniest microcontroller that you can think of.. PIC16Fxyz .
                 AuxCom_OnReceptionFromNetworkPortTunnel( // here: called from CwNet.c : CwNet_OnReceive()
                     pCwNet, // [in] "CW Network instance" that received the byte (not sure if AuxComPorts.c really needs to know this; but anyways..)
                     pClient->iCurrentCommand-CWNET_CMD_SERIAL_PORT_TUNNEL_1, // [in] iTunnelIndex
                     b ); // [in] received data byte, demultiplexed from the TCP/IP stream
                 break; // end case CWNET_CMD_SERIAL_PORT_TUNNEL
#            endif // SWI_NUM_AUX_COM_PORTS > 0 ?

              default : // any other / future command is ignored here,
                 // until all was received to call CwNet_ExecuteCmd();
                 // UNLESS it's "garbage" from the first received segment,
                 // e.g. someone has found the server's listening port
                 // and tries to e.g. "hack" an HTTP- or whatever server :
                 if( pClient->dwNumSegmentsRcvd == 1 )
                  { // 'Garbage' in the FIRST received TCP segment ...
                    pClient->fDisconnect = TRUE; // "kick him out" !
                  } // end if < FIRST received data in the entire client/server session > ?
                 else  // just COUNT this as "garbage"; most likely this indicates
                  {    // a problem with the streaming parser, after adding new commands:
                    if( pClient->nGarbageBlocksReceived < 10 )
                     { char sz255[256];
                       ++pClient->nGarbageBlocksReceived;
                       INET_DumpTraffic_HexOrASCII( sz255, 100, pClient->bCmdDataBuffer, pClient->iCmdDataLength );
                       ShowError( ERROR_CLASS_RX_TRAFFIC | SHOW_ERROR_TIMESTAMP,
                           "CwNet, Client%d: Unexpected 'Command with data block': 0x%02X (%s), data=%s",
                           (int)iClientIndex,
                           (unsigned int)pClient->iCurrentCommand,
                           (char*)CwNet_CommandFromBytestreamToString( (BYTE)pClient->iCurrentCommand ),
                           sz255 );
                       // 2025-09-14 : Trouble with the following...
                       // > 10:15:15.2 CwNet, Client1: Unexpected 'Command with data block': 0x06 (RigCtlD), data=[000B] s<010000>T7<0000>T7<00> ...
                       // > 10:15:15.2 CwNet, Client1: Unexpected 'Command with data block': 0x06 (RigCtlD), data=[000B] set_ptt 1\n<00>
                       // Fixed by adding 'case CWNET_CMD_RIGCTLD' as a dummy, a few lines further above.
                       // 'RigCtlD'-commands (whatever the "D" stands for) are not executed HERE,
                       // but in CwNet_ExecuteCmd() when the command has been received COMPLETELY.
                       // After that: OK, only the *execution* of 'RigCtlD'-commands, and the sent responses were displayed:
                       // > 10:34:38.7 RX1 Rigctld [000A] set_ptt 1\n
                       // > 10:34:38.7 TX1 Rigctld [0007] RPRT 0\n
                       // > 10:34:40.2 TX1 IntPara SMeterLevel = 2 dB
                       // > 10:34:41.4 TX1 IntPara SMeterLevel = 3 dB
                       // > 10:34:42.1 TX1 IntPara SMeterLevel = 2 dB
                       // > 10:34:42.6 RX1 Rigctld [000A] set_ptt 0\n
                       // > 10:34:42.6 TX1 Rigctld [0007] RPRT 0\n
                     } // end if( pClient->nGarbageBlocksReceived < 10 )
                  }   // end else < pClient->dwNumSegmentsRcvd != 1 >
                 break;
            } // end switch <CurrentCommand> in CWNET_PSTATE_RX_BLOCK
           if( pClient->iCmdDataByteIndex >= pClient->iCmdDataLength )
            { // Regardless if the command's DATA BLOCK fits in bCmdDataBuffer[] or not,
              // the last byte belonging to it has been received,
              // so it's time to "execute" the command - whatever it was:
              if( pClient->iCmdDataByteIndex < CWNET_CMD_DATA_BUFFER_SIZE )
               { // provide a trailing zero to treat pClient->bCmdDataBuffer
                 // as a C-string in CwNet_ExecuteCmd() :
                 pClient->bCmdDataBuffer[pClient->iCmdDataByteIndex] = '\0';
               }
              CwNet_ExecuteCmd( pCwNet, pClient );
              pClient->iRxParserState = CWNET_PSTATE_RX_CMD; // "done", expect the NEXT command
            }
           break; // end case case CWNET_PSTATE_RX_BLOCK
      } // end switch( pClient->iRxParserState )
   } // end while( nBytesRcvd-- )
  HERE_I_AM__CWNET();
} // end CwNet_OnReceive()

#if( SWI_USE_VORBIS_STREAM ) // allow streaming audio via Ogg/Vorbis ?
//---------------------------------------------------------------------------
void CwNet_Server_UpdateVorbisStreamIfNecessary( T_CwNet *pCwNet )
  // Periodically called from ServerThread() (every 20..40 ms).
  // If ANY of the currently connected clients 'wants a Vorbis audio stream',
  // this function keeps a buffer with the MOST RECENT OGG PAGES up to date.
  //   [in]  pCwNet->pCwDSP->sInputFifo : audio signal provided by CwDSP.c
  //   [out] pCwNet->sVorbis  : Ogg/Vorbis steam, "encoded ONCE, read by MULTIPLE clients"
{
  BOOL  fNeedVorbis = FALSE;
  int   i, nSamples;
  long  i32Result;
  static BOOL first_error = TRUE;
  // The SOURCE for audio streaming is pCwNet->pCwDSP->sInputFifo,
  //     with a sampling rate of CWDSP_INPUT_FIFO_SAMPLING_RATE (e.g. 8 kHz).
  // With 40 milliseconds per call from ServerThread(), the following
  // buffer (to read from the DSP's circular FIFO) should be sufficient:
#define L_TEMP_SAMPLE_BUFFER_SIZE (CWDSP_INPUT_FIFO_SAMPLING_RATE/10) // <- 100 milliseconds of "audio"
  float fltTemp[L_TEMP_SAMPLE_BUFFER_SIZE];
  T_CwNetClient *pClient;  
  for(i=0; i<=/*!*/CWNET_MAX_CLIENTS; ++i)
   { pClient = &pCwNet->Client[i];
     if( pClient->fWantVorbis ) // any of the <CWNET_MAX_CLIENTS> remote clients currently wants Ogg/Vorbis audio !
      { fNeedVorbis = TRUE;
        (void)fNeedVorbis; // "assigned a value that is never used" .. oh, shut up, Mr Pedantic !
      }
     else // this client is either PASSIVE of doesn't "want" Vorbis-audio in Ogg-containers:
      { pClient->dwNextVorbisPage = pCwNet->sVorbis.dwPageIndex+1; // avoid starting to play "in the past"
      }
   } // end for <iClient>

#if(0) // (0)=normal compilation, (1)=test to see the Vorbis-encoder "in action" even with NO CLIENT CONNECTED
  fNeedVorbis = TRUE;
#endif

  if(   fNeedVorbis // at least one of the currently connected clients "needs Vorbis"..
    && (pCwNet->sVorbis.nInitialHeaderBufferedBytes > 0)  // .. and VorbisStream_InitEncoder() was successful ..
    && (pCwNet->pCwDSP != NULL) )  // .. and the "CW DSP" can provide samples ..
   { // Proceed as described in VorbisStream.h,
     // > Ogg/Vorbis ENCODER API ("compress once, send to multiple receivers") .
     nSamples = CwDSP_GetNumSamplesInFifoForTailIndex( &pCwNet->pCwDSP->sInputFifo, pCwNet->iVorbisAudioTail );
     // Seen here after hitting a breakpoint below; thread was running for some time:
     //   nSamples=0, 250, 91, 341, 433, 250.   Theory says 20 ms * 8 kHz = 160 samples per call on average. Ok.
     if( nSamples > L_TEMP_SAMPLE_BUFFER_SIZE )
      {  nSamples = L_TEMP_SAMPLE_BUFFER_SIZE;
      }
     if( nSamples > 16 )  // enough samples to be "worth the effort"
      { CwDSP_ReadFromFifo( &pCwNet->pCwDSP->sInputFifo, &pCwNet->iVorbisAudioTail,
                            fltTemp, nSamples, NULL/*pdblTimestamp_s*/ );
        i32Result = VorbisStream_EncodeSamples( // .. into VORBIS audio, in OGG "pages" ...
           &pCwNet->sVorbis, // [out] the Ogg/Vorbis encoder's internal buffer
           fltTemp,         // [in] source buffer, a simple array, not a circular FIFO
           1,               // [in] number of audio channels PER SAMPLE POINT in pfltSrc
           nSamples );      // [in] number of sample points in the source buffer
        if( i32Result < 0 ) // oops.. got an error from the Ogg/Vorbis encoder ..
         { if(first_error )
            { first_error = FALSE;
              ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "VorbisEncoder: %s",
                 pCwNet->sVorbis.sz255ErrorString );
            }
         }
        // After a couple of calls to VorbisStream_EncodeSamples() [above],
        //  pCwNet->sVorbis.dwPageIndex should increment .
        //  Then, the most recent Ogg pages with Vorbis-encoded audio
        //  will be available in pCwNet->sVorbis.BufferPages[],
        //  and VorbisStream_GetNextPagesFromEncoder() will deliver them
        //  to any of the stream receivers .

      }
   } // end if( fNeedVorbis )
} // end CwNet_Server_UpdateVorbisStreamIfNecessary()
#endif // SWI_USE_VORBIS_STREAM ?

//---------------------------------------------------------------------------
static DWORD WINAPI ServerThread( LPVOID lpParam )
  // SERVER THREAD .. principle explained in
  //                  "Socket-based async multi-client tcp server.txt" .

{
  T_CwNet *pCwNet = (T_CwNet*)lpParam; // address of our instance data (struct)
     // ,--'
     // '--> expect this to be &MyCwNet = the instance prepared in Keyer_Main.cpp .
  T_CwNetClient *pClient;
  SOCKET master, client_socket[CWNET_MAX_CLIENTS], s;
    // '---> In winsock2, a SOCKET is a "UINT_PTR" - NOT AN INTEGER !
    //       But don't assume it's a POINTER - thus when invalid,
    //       any of these variables must not be NULL or 0 but INVALID_SOCKET !
  struct sockaddr_in server, address;
  int activity, addrlen, i, n, valread, iClientIndex;
  TIMEVAL timeout; // funny thing with SECONDS and MICROSECONDS, for select()

  // set of socket descriptors
  fd_set readfds;
  int max_sd;   // "highest file descriptor number" for select() [Berkeley only, ignored by winsock]
  BOOL fOk, fUpdateStatistics = FALSE;
  int  iSimBadConnTime_ms = 0;

  // Debugging stuff .. may kick this out one fine day:
  T_TIM_Stopwatch swLoopTime, swSimBadConn;
  char sz1kDump[1024];

  HERE_I_AM__CWNET(); // <- for post-mortem analysis, or thread-crash-analysis in the GUI

  pCwNet->iThreadStatus = CWNET_THREAD_STATUS_RUNNING; // cq cq, this is ServerThread() calling and standing by ..

  for(i=0; i<CWNET_MAX_CLIENTS; i++)
   { client_socket[i] = INVALID_SOCKET;  // INVALID_SOCKET, not zero
     // (who nows if 'zero' isn't a VALID socket - don't assume anything)
   }

  // Create the server's "listening" socket (on the 'published' port number)
  if((master = socket(AF_INET , SOCK_STREAM , 0 )) == INVALID_SOCKET)
   { // shit happens .. try to report this somewhere :
     int error_code = WSAGetLastError();  // "quickly" get the error code, before calling any other 'WSA'-thing.. (*)
     // (*) Somehow, WSAGetLastError() must be aware of the 'caller' (thread ID?)..
     sprintf( pCwNet->sz255LastError, "CW-Server: could not create socket (%s)",
                                       INET_WinsockErrorCodeToString( error_code ) );
     pCwNet->pszLastError = pCwNet->sz255LastError;
     pCwNet->iThreadStatus = CWNET_THREAD_STATUS_TERMINATED; // flag for the application
     if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
      { ShowError( ERROR_CLASS_ERROR, pCwNet->sz255LastError );
      }
     HERE_I_AM__CWNET();
     return -1; // rest in peace, ServerThread() ... running without a LISTENING socket is useless
   }

  // Prepare the sockaddr_in structure
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = INADDR_ANY;
  server.sin_port = htons( pCwNet->cfg.iServerListeningPort );

  // Bind the server's "listening socket" to our well-known (published) TCP port:
  if( bind(master ,(struct sockaddr *)&server , sizeof(server)) == SOCKET_ERROR)
   { // shit happens ...
     sprintf( pCwNet->sz255LastError, "CW-Server: could not bind socket (%s)",
              INET_WinsockErrorCodeToString( WSAGetLastError() ) );
     pCwNet->pszLastError = pCwNet->sz255LastError;
     pCwNet->iThreadStatus = CWNET_THREAD_STATUS_TERMINATED; // flag for the application
     if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
      { ShowError( ERROR_CLASS_ERROR, pCwNet->sz255LastError );
      }
     HERE_I_AM__CWNET();
     return -1; // farewell, ServerThread() ... no server operation after "bind()" failed
   }

  // Begin listening to incoming connections
  if( listen(master , 3/*"backlog"*/ ) == SOCKET_ERROR)
   { // shit .. the 'socket services' refused to start LISTENING for TCP/IP "connect" requests on the server port !
     snprintf( pCwNet->sz255LastError, 200, "CW-Server: can't listen on port %d (%s)",
              (int)pCwNet->cfg.iServerListeningPort,
              INET_WinsockErrorCodeToString( WSAGetLastError() ) );
     pCwNet->pszLastError = pCwNet->sz255LastError;
     pCwNet->iThreadStatus = CWNET_THREAD_STATUS_TERMINATED; // flag for the application
     if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
      { ShowError( ERROR_CLASS_ERROR, pCwNet->sz255LastError );
      }
     HERE_I_AM__CWNET();
     return -1; // farewell, ServerThread() ... no server operation after "listen" failed
   }

  if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
   { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "CW-Server: thread running.." );
   }

  TIM_StartStopwatch( &swSimBadConn );

  // Accept incoming connections, and 'serve' remote client in this loop ...
  while( (pCwNet->iThreadStatus == CWNET_THREAD_STATUS_RUNNING )
       &&(pCwNet->cfg.iFunctionality == CWNET_FUNC_SERVER ) // only run as long as this instance SHALL BE server
       )
   {
     HERE_I_AM__CWNET();
     ++pCwNet->dwThreadLoops;  // Does this thread "sleep" most of the time, or is it a CPU hog ?
     pCwNet->dw8ThreadIntervals_us[ pCwNet->dwThreadLoops & 7 ] = TIM_ReadStopwatch_us( &swLoopTime );
     TIM_StartStopwatch( &swLoopTime );

#   if( SWI_USE_VORBIS_STREAM )
     // If one or more currently connected clients 'wants' Ogg/Vorbis-compressed
     // audio, read input from the DSP's streaming buffer, and compress for streaming:
     CwNet_Server_UpdateVorbisStreamIfNecessary( pCwNet );
#   endif // SWI_USE_VORBIS_STREAM ?

     CwNet_DuplicateParameterQueueForRemoteClients( pCwNet );


     if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SIMULATE_BAD_CONN )
      { // Special option : "simulate a bad network connection",
        // with large latencies, and A LOT OF UNPREDICTABLE JITTER on the latency:
        // fSimulateBadConnNow will be set for anything from 40 to ~300 milliseconds,
        //  during which this thread WILL NOT POLL FOR TRANSMISSION,
        //  and will not send() anything (even if there is something to send).
        if( TIM_ReadStopwatch_ms( &swSimBadConn ) > iSimBadConnTime_ms )
         { // Throw the dice for the NEXT 'jittering latency time' again
           //  (or the time during which the network thread reacts IMMEDIATELY)
           iSimBadConnTime_ms = 40 + ( (rand() >> 7) & 255/*ms*/ );
           TIM_StartStopwatch( &swSimBadConn );
           pCwNet->fSimulateBadConnNow = !pCwNet->fSimulateBadConnNow;
         }
      } // end if < CWNET_DIAG_FLAGS_SIMULATE_BAD_CONN > ?
     else  // ! CWNET_DIAG_FLAGS_SIMULATE_BAD_CONN ->
      { pCwNet->fSimulateBadConnNow = FALSE;
      }


     // Forget old flags BEFORE EVERY CALL OF SELECT, at least for the 'set'
     // of sockets on which we want to wait. Seen somewhere on the net:
     // > It's important that you re-enable the file descriptors that were cleared
     // > prior to starting another select, otherwise, you will no longer be
     // > waiting on those file descriptors.
     FD_ZERO(&readfds);

     // add master socket to fd set
     FD_SET(master, &readfds);
     // > FD_SET places sockets into a "set" for various purposes, such as
     // > testing a given socket for readability using the readfds parameter
     // > of the select function.

     // add child sockets to fd set
     for( max_sd=i=0 ; i <CWNET_MAX_CLIENTS; i++)
      {
        s = client_socket[i];
        if(s != INVALID_SOCKET)
         { FD_SET( s , &readfds);
         }
        // highest file descriptor number; Berkeley/Linux needs this for select()
        if((int)s > max_sd)
         { max_sd = (int)s;  // indeed the maximum "SOCKET" value, not the array index 'i' !
           // (guess in Berkeley, a SOCKET is indeed an array index,
           //  not a 'handle' / 'pointer'-like thingy that is possibly is in winsock)
         }
      } // end for < add child sockets to fd set >

     // Prepare waiting for an activity on any of the sockets.
     // Unlike the simple server example, our timeout for select()
     // isn't NULL, because we do NOT want to wait indefinitely ('endlessly'):
     timeout.tv_sec  = 0;     // Pfui Deibel.. warum einfach, wenn's auch kompliziert geht !
     timeout.tv_usec = CWNET_SOCKET_POLLING_INTERVAL_MS * 1000; // max time to wait in MICROSECONDS !
     HERE_I_AM__CWNET();
     activity = select( max_sd+1, &readfds , NULL , NULL , &timeout);
     //           ,-----'           |          |      |
     //           |   ,-------------'          |      |
     //           |   | ,----------------------'      |
     //           |   | | ,---------------------------'
     //           |   | | |
     //           |   | | An optional pointer to a set of sockets to be checked for errors.
     //           |   | An optional pointer to a set of sockets to be checked for writability.
     //           |   An optional pointer to a set of sockets to be checked for readability.
     //           |
     //           Ignored by Winsock ("nfds" is a UNIX thing, something like
     //           "the highest dont-know-what (SOCKET? HANDLE? INDEX?) PLUS ONE")
     HERE_I_AM__CWNET();
     // > The select function returns the total number of socket handles
     // > that are ready and contained in the fd_set structures,
     // > zero if the time limit expired, or SOCKET_ERROR
     // > if an error occurred. If the return value is SOCKET_ERROR,
     // > WSAGetLastError can be used to retrieve a specific error code.
     // > A set of macros is provided for manipulating an fd_set structure.
     // > These macros are compatible with those used in the Berkeley software,
     // > but the underlying representation is completely different.
     // >
     // > The parameter readfds identifies the sockets that are to be checked
     // > for readability. If the socket is currently in the listen state,
     // > it will be marked as readable if an incoming connection request
     // > has been received such that an accept is guaranteed to complete
     // > without blocking. For other sockets, readability means that queued
     // > data is available for reading such that a call to recv, WSARecv,
     // > WSARecvFrom, or recvfrom is guaranteed not to block.
     if( activity == 0 )  // > "zero if the time limit expired"...
      { // .. which means NOTHING HAPPENED on any socket;
        //    so don't waste time checking FD_ISSET() below,
        //    just loop around and sleep for another 20 ms in select() .
        //    (only in THIS particular case, don't forget to POLL all
        //     currently opened sockets; maybe some of them have something
        //     TO SEND; even without having received anything.
        //     Mr. Nagle will possibly be not amused; but there's not much
        //     we can do (without digging in deeply) to make him cheer up) :
        HERE_I_AM__CWNET();
        for( i=0; (!pCwNet->fSimulateBadConnNow) && (i<CWNET_MAX_CLIENTS); i++)
         {
           s = client_socket[i];
           // With 'activity==0', we know NONE of the sockets is readable,
           //      so don't check for RECEPTION - only "poll" for TRANSMISSION:
           if( s != INVALID_SOCKET )
            { // here's a VALID socket, so poll the application for TRANSMISSION :
              iClientIndex  = i+CWNET_FIRST_REMOTE_CLIENT_INDEX;
              pClient = &pCwNet->Client[iClientIndex];
              pCwNet->nBytesInTxBuffer = 0; // nothing appended to pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ] yet ..
                  // (Note: There is only ONE COMMON pCwNet->bTxBuffer,
                  //        used for MULTIPLE remote clients in this loop !
                  //        Thus, anything that CwNet_OnPoll() may allocate
                  //        and assemble for transmission via CwNet_AllocBytesInTxBuffer()
                  //        MUST be sent() further below.
              CwNet_OnPoll( pCwNet, pClient );   // here: called from ServerThread()
              if( pCwNet->nBytesInTxBuffer > 0 ) // IF there's anything to send..
               { // send whatever CwNet_OnPoll() may have
                 // appended to pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ],
                 // with a maximum of <CWNET_SOCKET_TX_BUFFER_SIZE> :
                 if( send(s, (const char *)pCwNet->bTxBuffer, pCwNet->nBytesInTxBuffer, 0)
                                             != pCwNet->nBytesInTxBuffer )
                  { pCwNet->pszLastError = "ServerThread failed to 'send()' after 'poll'";
                    ShowError( ERROR_CLASS_ERROR | SHOW_ERROR_TIMESTAMP, pCwNet->pszLastError );
                  }
                 else // successfully SENT something, so COUNT this (for statistics):
                  { pCwNet->dwNumBytesSent  += pCwNet->nBytesInTxBuffer; // .. for ALL clients (entire server)
                    pClient->dwNumBytesSent += pCwNet->nBytesInTxBuffer; // .. and PER CONNECTION (socket)
                    CwServer_FeedActivityWatchdog( pClient ); // feed the dog .. here: after successful send()
#                  if(0) // Removed 2025-01, because with AUDIO, SPECTRA, and who-knows-what, the traffic was TOO MUCH..
                    if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                     { INET_DumpTraffic_HexOrASCII( sz1kDump,500, pCwNet->bTxBuffer, pCwNet->nBytesInTxBuffer );
                       ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d %s",
                                   (int)iClientIndex, sz1kDump );
                     }
#                  endif // (0) .. removed 2025-01
                    pCwNet->nBytesInRxBuffer = 0;
                  }
               } // end if < CwNet_OnPoll() deposited something in the TX-buffer >
            }   // end if < valid socket for POLLING *without* previous reception >
         }     // end for (i=0; i<CWNET_MAX_CLIENTS; i++)
      } // end if < activity==0, which means NO ERROR but NO RECEPTION on any socket >
     else if (activity < 0) // > "SOCKET_ERROR (-1) if an error occurred"...
      { // select() failed.  "This is a permanent error" ... close the socket, give up.
        pCwNet->pszLastError = "ServerThread terminated after 'select()' failed";
        pCwNet->iThreadStatus = CWNET_THREAD_STATUS_TERMINATED; // flag for the application
        if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
         { ShowError( ERROR_CLASS_ERROR | SHOW_ERROR_TIMESTAMP, pCwNet->pszLastError );
         }
        HERE_I_AM__CWNET();
        return -1; // farewell, ServerThread() ... after a "permanent" error with select() [yet another "socket" operation]
      }
     else // neither "timeout" in select() or a "complete fail" -> SOMETHING must have happened !
      { // If something happened on the master socket (aka 'listening' socket), then its an incoming connection
        if (FD_ISSET(master , &readfds))
         { SOCKET new_socket;
           // Even a WEB BROWSER can be used to test this "locally":
           //  In Firefox, enter "localhost:7355" in the address field.
           //  The Fox tries to be smart and replaces this by something like
           //                    "http://localhost:7355/" - oh well...
           //  (we don't speak HTTP but 'raw TCP', but anyway, good for a test)
           addrlen = sizeof(struct sockaddr_in);
           HERE_I_AM__CWNET();
           new_socket = accept(master , (struct sockaddr *)&address, (int *)&addrlen);
           HERE_I_AM__CWNET();
           if( (new_socket != INVALID_SOCKET )  // Only "let him in" if he's not a bad guy ..
             &&(!INET_IsBlockedIPv4Address( &pCwNet->sBlacklist, &address.sin_addr.S_un.S_un_b.s_b1 ) )
             )
            { // accept() successful ... we've got a NEW, "accepted" socket ->
              // > inform user of socket number - used in send and receive commands
              // ex: printf("New connection , socket fd is %d , ip is : %s , port : %d \n" , new_socket , inet_ntoa(address.sin_addr) , ntohs(address.sin_port));

              // add new socket to array of sockets ..
              iClientIndex = -1;
              for( i=0; i<CWNET_MAX_CLIENTS; i++)
               {
                 if( (client_socket[i] == INVALID_SOCKET) && (iClientIndex<0) )
                  { // Bingo, found the first UNUSED entry, so use it for the new client:
                    client_socket[i] = new_socket;
                    iClientIndex = i + CWNET_FIRST_REMOTE_CLIENT_INDEX;
                    new_socket = INVALID_SOCKET; // "moved" the socket handle into client_socket[] so don't "
                    break;
                  }
               } // end for < add new socket to array of sockets >
              fUpdateStatistics = TRUE; // update pCwNet->nRemoteClientsConnected a.s.a.p.
              if( iClientIndex >= CWNET_FIRST_REMOTE_CLIENT_INDEX ) // Retrieve some info about the new peer, then say hello...
               { pClient = &pCwNet->Client[iClientIndex];
                 // Extract what we need to know about the remote client
                 //   in less exotic structs than "struct sockaddr_in", etc,
                 //   and (even more important) in THE HOST'S byte order :
                 memset( (void*)pClient, 0, sizeof(T_CwNetClient) ); // clear counter, state, list of Unified Parameter Numbers to send to this guy, etc
                 pClient->dwMagicPI = CWNET_MAGIC_PI;  // repair the "magic pie" immediately (without this, CwNet_SanityCheck() immediately failed. Ok.)
                 pClient->b4HisIP.b[0] = address.sin_addr.S_un.S_un_b.s_b1; // "first byte" of HIS IP-v4-address
                 pClient->b4HisIP.b[1] = address.sin_addr.S_un.S_un_b.s_b2; // ..
                 pClient->b4HisIP.b[2] = address.sin_addr.S_un.S_un_b.s_b3;
                 pClient->b4HisIP.b[3] = address.sin_addr.S_un.S_un_b.s_b4; // "fourth byte" of HIS IP-v4-address
                 pClient->iHisPort   = ntohs(address.sin_port); // in this case, "his" port is an EPHEMERAL port number
                 HERE_I_AM__CWNET();
                 CwServer_OnConnect( pCwNet, pClient ); // WE (server) have been connected by a remote client..
                 HERE_I_AM__CWNET();
                 // do NOT start sending from this end .. the other guy may be a WEB BROWSER or a bad guy !
               } // end if( iNewClientIndex >= 0 )
            }   // end if < new (accepted) socket VALID > ?
           if( new_socket != INVALID_SOCKET ) // not moved into the array or closed yet ?
            { closesocket( new_socket );
            }
           // end of the existance of 'SOCKET new_socket;'
         } // end if (FD_ISSET(master , &readfds)) ..

        // ..else its some IO operation on some other socket :)
        for( i=0; i<CWNET_MAX_CLIENTS; i++)
         {
           s = client_socket[i];
           iClientIndex  = i+CWNET_FIRST_REMOTE_CLIENT_INDEX;
           pClient = &pCwNet->Client[iClientIndex];

           pCwNet->nBytesInTxBuffer = 0; // nothing appended to pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ]
           // for THIS remote client in THIS server-thread-loop yet ...

           // If the client is present in the 'readable sockets',  READ (call "recv()") !
           if( (s != INVALID_SOCKET) &&  (FD_ISSET( s , &readfds)) )
            {
              // get details of the client (geek name = "peer" = "the guy at the other end of the socket")
              addrlen = sizeof(struct sockaddr_in);
              getpeername(s , (struct sockaddr*)&address , (int*)&addrlen);

              // Check if it was for closing, and also read the incoming message
              HERE_I_AM__CWNET();
              valread = recv( s, (char *)pCwNet->bRxBuffer, CWNET_SOCKET_RX_BUFFER_SIZE, 0);
              // ,---------------------------------------------------------------'
              // '--> From the Winsock specification (about the last argument):
              // > A set of flags that influences the behavior of this function.
              // Zero if we don't want to "peek" (read without removing from the
              // network buffer), don't want "Out Of Band"-data, etc etc.
              if( valread == SOCKET_ERROR)
               {
                 int error_code = WSAGetLastError();
                 if(error_code == WSAECONNRESET)
                  { HERE_I_AM__CWNET();
                    // Somebody disconnected , get his details and print
                    // ex: printf("Host disconnected unexpectedly , ip %s , port %d \n" , inet_ntoa(address.sin_addr) , ntohs(address.sin_port));
                    CwServer_OnDisconnect( pCwNet, pClient );

                    // Close the socket and mark as 0 in list for reuse
                    closesocket( s );
                    client_socket[i] = INVALID_SOCKET;
                    fUpdateStatistics = TRUE; // update pCwNet->nRemoteClientsConnected a.s.a.p.
                  }
                 else // "SOCKET_ERROR" but not "WSAECONNRESET" ?
                  { HERE_I_AM__CWNET();
                    if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
                     { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Recv failed, IP %s (%s)",
                        CwNet_IPv4AddressToString( (BYTE*)&address.sin_addr.S_un.S_un_b.s_b1 ),
                        INET_WinsockErrorCodeToString( error_code ) );
                     }
                  }
               } // end if recv() -> SOCKET_ERROR ?
              if ( valread == 0)
               {
                 // Somebody disconnected , get his details and print
                 // ex: printf("Host disconnected , ip %s , port %d \n" , inet_ntoa(address.sin_addr) , ntohs(address.sin_port));
                 HERE_I_AM__CWNET();
                 CwServer_OnDisconnect( pCwNet, pClient );

                 // Close the socket and mark it's handle as INVALID in list, for reuse
                 closesocket( s );
                 client_socket[i] = INVALID_SOCKET;
                 fUpdateStatistics = TRUE; // update pCwNet->nRemoteClientsConnected a.s.a.p.
               }
              else // something received on this client socket ->  pass it on...
               { pCwNet->nBytesInRxBuffer = valread;
                 if( pCwNet->nBytesInRxBuffer > pCwNet->iPeakRxBufferUsage )
                  {  pCwNet->iPeakRxBufferUsage = pCwNet->nBytesInRxBuffer;
                  }
                 if( valread > 0 )
                  { pCwNet->dwNumBytesRcvd += valread; // here: inc'd in ServerThread()
                  }
                 // When tested with a WEB BROWSER (to replace the not-yet-functional
                 // "Remote CW"-client), got here with valread above 400 bytes
                 //      (!  ... getting more with each browser update ... ),
                 //   and pCwNet->bRxBuffer = "GET / HTTP/1.1\r\n" ...
                 //
                 // To make Nagle's-Algorithm happy, it's important
                 // to call send() only ONCE(!) after a successfull recv() .
                 // So here's THE ONLY PLACE where we pass the received segment
                 // to the application, and let some subroutine decide if,
                 // and what to send "as a response" (from server to client):
                 // add null character, if you want to use with printf/puts or other string handling functions
                 HERE_I_AM__CWNET();
                 CwNet_OnReceive( pCwNet, pClient, pCwNet->bRxBuffer, pCwNet->nBytesInRxBuffer );
                 HERE_I_AM__CWNET();
               }
            } // end if (FD_ISSET( s , &readfds) )

           // Regardless if something was received on this socket or not,
           //  poll for TRANSMISSION (here on the SERVER SIDE, possibly for MULTIPLE sockets):
           if( s != INVALID_SOCKET )  // remember, we're still in the "for-all-remote-clients"-loop..
            { HERE_I_AM__CWNET();
              CwNet_OnPoll( pCwNet, pClient ); // here: also called from ServerThread()
              HERE_I_AM__CWNET();
              if( pCwNet->nBytesInTxBuffer > 0 ) // IF there's anything to send..
               { // send whatever all those functions called above may have
                 // appended to pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ],
                 // with a maximum of <CWNET_SOCKET_TX_BUFFER_SIZE> in bytes.
                 if( send( s, (const char *)pCwNet->bTxBuffer, pCwNet->nBytesInTxBuffer, 0)
                                              != pCwNet->nBytesInTxBuffer )
                  { pCwNet->pszLastError = "ServerThread failed to 'send()'";
                  }
                 else // successfully SENT something, so COUNT this (for statistics):
                  { pCwNet->dwNumBytesSent  += pCwNet->nBytesInTxBuffer;
                    pClient->dwNumBytesSent += pCwNet->nBytesInTxBuffer;
                    CwServer_FeedActivityWatchdog( pClient ); // feed the dog .. here: after successful send()
#                  if(0) // Removed 2025-01, because with all those AUDIO- and SPECTRUM data, too much for the 'Debug' tab !
                    if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                     { INET_DumpTraffic_HexOrASCII( sz1kDump,500, pCwNet->bTxBuffer, pCwNet->nBytesInTxBuffer );
                       ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX%d %s",
                                   (int)iClientIndex, sz1kDump );
                     }
#                  endif // removed the 'hex dump of EVERYTHING' in 2025-01
                    pCwNet->nBytesInTxBuffer = 0;
                  }
               } // end if < something to send after CwNet_OnPoll() >
              if( pClient->fDisconnect ) // anything called above decided to DISCONNECT :
               { // (Note: Without this, an HTTP response with an error was NOT displayed
                 //        in a web browser, as if NOTHING had been sent from here at all.
                 //  The myths of HTTP and "modern browsers" ... see HttpServer.c )
                 s = client_socket[i];  // socket handle still valid ?
                 if( s != INVALID_SOCKET )
                  { // Close the socket and mark as 0 in list for reuse
                    closesocket( s );
                    client_socket[i] = INVALID_SOCKET;
                    fUpdateStatistics = TRUE; // update pCwNet->nRemoteClientsConnected a.s.a.p.
                  }
                 pClient->fDisconnect = FALSE; // "done" (have disconnected and closed the socket)
                 pClient->iClientState= CWNET_CLIENT_STATE_DISCONN;
               }
            } // end if( pClient->fDisconnect )
         }   // end for (i=0; i<CWNET_MAX_CLIENTS; i++)
      }     // end else <neither "timeout"  or a "complete fail" in select() >

     // Even if there's nobody connected to the server, periodically check
     // for activity 'on the key' (or, maybe later, 'on the microphone')
     // because THE SERVER ITSELF may operate as a 'local keyer',
     // and the indicator field labelled 'On the key now :' (on the 'Network' tab)
     // shall toggle between 'The Sysop' and '-- nobody --' for testing !
     if( pCwNet->iTransmittingClient >= CWNET_LOCAL_CLIENT_INDEX ) // check for "no activity on the key" ?
      { if( TIM_ReadStopwatch_ms( &pCwNet->swTransmittingClient ) > 1000 ) // ToDo: Make this configurable ..
         { HERE_I_AM__CWNET();
           CwNet_SwitchTransmittingClient( pCwNet, -1/*"--nobody--"*/ );
         }
      }

     // Let the CW-net-server send an updated set of band-stacking registers,
     // to all currently connected clients:
     if(pCwNet->RigControl.fBandStackingRegsModifiedForCwNet )
      { pCwNet->RigControl.fBandStackingRegsModifiedForCwNet = FALSE; // here: cleared in CwNet.c : ServerThread()
        for( i=0; i<CWNET_MAX_CLIENTS; i++)
         { iClientIndex = i+CWNET_FIRST_REMOTE_CLIENT_INDEX;
           pClient = &pCwNet->Client[iClientIndex];
           pClient->iBandStackingRegsTxIndex = 0; // activate sending band-stacking-registers -> the rest happens in CwNet_OnPoll()
         }
      } // end if( pCwNet->RigControl.fBandStackingRegsModifiedForCwNet )

     if( fUpdateStatistics ) // update "statistics" (mostly for the GUI) ?
      { for( n=i=0; i<CWNET_MAX_CLIENTS; i++)
         { if( client_socket[i] != INVALID_SOCKET )
            { ++n;
            }
         }
        pCwNet->nRemoteClientsConnected = n; // <- for "diagnostics", read in another thread
        fUpdateStatistics = FALSE; // "done"
      } // end if( fUpdateStatistics )
   }   // end while < server-thread shall run >

  HERE_I_AM__CWNET();
  for(i=0; i<CWNET_MAX_CLIENTS; i++)
   { if( client_socket[i] != INVALID_SOCKET )
      { closesocket( client_socket[i] );
      }
   }

  if( master != INVALID_SOCKET )
   { closesocket( master );
   }
  if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
   { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "CW-Server: Disconnected and thread terminated" );
   }

  HERE_I_AM__CWNET();
  return 0;
} // end ServerThread()


//---------------------------------------------------------------------------
static void CwNet_CloseClientSocketAndStartWaitingForReconnect(
   T_CwNet *pCwNet, SOCKET *pSocket ) // exclusively called from ClientThread() !
{
  if( *pSocket != INVALID_SOCKET )
   { closesocket( *pSocket );
     *pSocket = INVALID_SOCKET;
   }
  pCwNet->iLocalClientConnState = CWNET_CONN_STATE_WAIT_RECONN;
  TIM_StartStopwatch( &pCwNet->swClientReconnTimer );
}

//---------------------------------------------------------------------------
static DWORD WINAPI ClientThread( LPVOID lpParam )
  // Our CLIENT THREAD .. more or less the 'remote counterpart' for ServerThread(),
  //                      but simpler because here's only ONE socket to manage.
{
  T_CwNet *pCwNet = (T_CwNet*)lpParam; // address of our instance data (struct)
  SOCKET  s = INVALID_SOCKET; // no NULL, not zero, but INVALID_SOCKET !
  int    iWSAerror;
  struct sockaddr_in server, address;
  int activity, addrlen, i, valread;
  TIMEVAL timeout; // funny thing with SECONDS and MICROSECONDS, for select()

  // set of socket descriptors
  fd_set readfds;
  int max_sd;   // "highest file descriptor number" for select() [Berkeley only, ignored by winsock]
  BOOL fOk;

  // Debugging stuff .. may kick this out one fine day:
  T_TIM_Stopwatch swLoopTime, swSimBadConn;
  int  iSimBadConnTime_ms = 0;
  char sz1kDump[1024];


  pCwNet->iThreadStatus = CWNET_THREAD_STATUS_RUNNING; // cq cq, this is ServerThread() calling and standing by ..
  pCwNet->iLocalClientConnState = CWNET_CONN_STATE_TRY_CONNECT;

  TIM_StartStopwatch( &pCwNet->swClientReconnTimer ); // .. for re-connect, etc

  // Loop to create a socket, connect, send and receive data ... here: CLIENT side
  while( ( pCwNet->iThreadStatus == CWNET_THREAD_STATUS_RUNNING )
      && ( pCwNet->cfg.iFunctionality == CWNET_FUNC_CLIENT ) // only run as long as this instance SHALL BE client
       )
   {
     ++pCwNet->dwThreadLoops;  // Does this thread "sleep" most of the time, or is it a CPU hog ?
     pCwNet->dw8ThreadIntervals_us[ pCwNet->dwThreadLoops & 7 ] = TIM_ReadStopwatch_us( &swLoopTime );
     TIM_StartStopwatch( &swLoopTime );

     if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SIMULATE_BAD_CONN )
      { // Special option : "simulate a bad network connection",
        // with large latencies, and A LOT OF UNPREDICTABLE JITTER on the latency:
        // fSimulateBadConnNow will be set for anything from 40 to ~300 milliseconds,
        //  during which this thread WILL NOT POLL FOR TRANSMISSION,
        //  and will not send() anything (even if there is something to send).
        if( TIM_ReadStopwatch_ms( &swSimBadConn ) > iSimBadConnTime_ms )
         { // Throw the dice for the NEXT 'jittering latency time' again
           //  (or the time during which the network thread reacts IMMEDIATELY)
           iSimBadConnTime_ms = 40 + ( (rand() >> 7) & 255/*ms*/ );
           TIM_StartStopwatch( &swSimBadConn );
           pCwNet->fSimulateBadConnNow = !pCwNet->fSimulateBadConnNow;
         }
      } // end if < CWNET_DIAG_FLAGS_SIMULATE_BAD_CONN > ?
     else  // ! CWNET_DIAG_FLAGS_SIMULATE_BAD_CONN ->
      { pCwNet->fSimulateBadConnNow = FALSE;
      }


     pCwNet->nBytesInTxBuffer = 0; // nothing appended to pCwNet->bTxBuffer[ pCwNet->nBytesInTxBuffer++ ]
                                   // IN THIS THREAD LOOP yet ...

     // Unlike many simple TCP CLIENT examples, we even try the "connect()"
     //  IN THE LOOP, so the client (-thread) has the chance to repeat his
     //  attempt to connect the remote server - for example, if the client
     //  application (instance) was launched BEFORE the server.
     //  Thus, here's another STATE MACHINE :
     switch( pCwNet->iLocalClientConnState )  // what's the next step ?
      { case CWNET_CONN_STATE_OFF :        //  .. nothing yet, just WAIT ..
           break;
        case CWNET_CONN_STATE_WAIT_RECONN: // waiting before trying to connect() again
           if( TIM_ReadStopwatch_ms( &pCwNet->swClientReconnTimer) > CWNET_RECONN_INTERVAL_MS )
            { pCwNet->iLocalClientConnState = CWNET_CONN_STATE_TRY_CONNECT;
            }
           break;
        default:
           break;
        case CWNET_CONN_STATE_TRY_CONNECT:
           // Parse whatever we have now (domain name or numeric IP, plus port number):
           INET_LookupDomainOrParseIP( pCwNet->cfg.sz80ClientRemoteIP,
                 pCwNet->Client[0].b4HisIP.b, &pCwNet->Client[0].iHisPort );

           // Prepare the sockaddr_in structure ..
           server.sin_family = AF_INET;
           server.sin_addr.S_un.S_un_b.s_b1 = pCwNet->Client[0].b4HisIP.b[0];
           server.sin_addr.S_un.S_un_b.s_b2 = pCwNet->Client[0].b4HisIP.b[1];
           server.sin_addr.S_un.S_un_b.s_b3 = pCwNet->Client[0].b4HisIP.b[2];
           server.sin_addr.S_un.S_un_b.s_b4 = pCwNet->Client[0].b4HisIP.b[3];
           server.sin_port = htons( pCwNet->Client[0].iHisPort );

           // Create a socket if not done yet (a TCP client only ever needs ONE):
           if( s == INVALID_SOCKET )
            {  s = socket(AF_INET , SOCK_STREAM , 0 );
            }
           if( s == INVALID_SOCKET) // oops.. a hopeless case; bail out
            { // report this 'unrecoverable error' somewhere:
              sprintf( pCwNet->sz255LastError, "ClientThread: could not create socket (%s)",
                        INET_WinsockErrorCodeToString( WSAGetLastError() ) );
              if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
               { ShowError( ERROR_CLASS_ERROR | SHOW_ERROR_TIMESTAMP, "%s", pCwNet->sz255LastError );
               }
              pCwNet->pszLastError = pCwNet->sz255LastError;
              pCwNet->iThreadStatus = CWNET_THREAD_STATUS_TERMINATED; // flag for the application
              return -1; // farewell, ClientThread() ... failed to create a TCP/IP 'socket' so cannot even try to CONNECT anyone
            }
           // Try to connect to the remote server:
           if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
            { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Client: Trying to connect %s:%d ...",
                         CwNet_IPv4AddressToString(pCwNet->Client[0].b4HisIP.b),
                         (int)pCwNet->Client[0].iHisPort );
            }
           if( connect( s, (struct sockaddr *)&server, sizeof(server)) < 0 )
            { // failed to connect() our remote "CW" server ->
              iWSAerror = WSAGetLastError();  // bizarre API .. why not directly return an error code ?
              if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
               { ShowError( ERROR_CLASS_ERROR | SHOW_ERROR_TIMESTAMP, " .. failed: %s",
                            INET_WinsockErrorCodeToString( iWSAerror ) );
               }
              sprintf( pCwNet->sz255LastError, "ClientThread: could not connect (%s)",
                       INET_WinsockErrorCodeToString( iWSAerror ) );
              pCwNet->pszLastError = pCwNet->sz255LastError;
              // In THIS case, keep the client thread running. Not being able to
              // connect() may be just a temporary hiccup. Try again a few
              // seconds later; maybe the network is back 'on-line' then.
              // Not sure if after a failed connect(), a socket can be recycled.
              // Assume it CANNOT, so:
              CwNet_CloseClientSocketAndStartWaitingForReconnect( pCwNet, &s );
              //     '--> CWNET_CONN_STATE_WAIT_RECONN
            }
           else // ok, connect() was successful, but that doesn't mean we're "logged in" ..
            {
              if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
               { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, " ..successfully connected to %s:%d",
                         CwNet_IPv4AddressToString(pCwNet->Client[0].b4HisIP.b),
                         (int)pCwNet->Client[0].iHisPort );
               }
              // In the next state(s), we may actively "communicate",
              //  e.g. repeatedly call select() and possibly recv() further below.
              pCwNet->iLocalClientConnState = CWNET_CONN_STATE_CONNECTED;
              // for the higher-level "client state machine" (with log-in, etc):
              pCwNet->Client[0].iClientState = CWNET_CLIENT_STATE_CONNECTED;
              //  ,-------------------------------------------------'
              //  '--> "connected but not yet logged in" !
            }
           break;
        case CWNET_CONN_STATE_CONNECTED :
           break;
      } // end switch( pCwNet->iLocalClientConnState )


     // 'Actively communicate' [send(), recv()] via the socket ?
     if(  ( s != INVALID_SOCKET )
        &&( pCwNet->iLocalClientConnState == CWNET_CONN_STATE_CONNECTED )
        &&( !pCwNet->fSimulateBadConnNow )
       )
      {
        FD_ZERO(&readfds);  // similar as in the SERVER thread, prepare to select()..
        FD_SET(s, &readfds);
        // > FD_SET places sockets into a "set" for various purposes, such as
        // > testing a given socket for readability using the readfds parameter
        // > of the select function.

        // Prepare waiting for an activity on any of the sockets.
        // Also here (in the CLIENT thread), we don't want to wait endlessly
        // for RECEPTION, because we may have other plans, too ..
        timeout.tv_sec  = 0; // Pfui Deibel.. warum einfach, wenn's auch kompliziert geht !
        timeout.tv_usec = CWNET_SOCKET_POLLING_INTERVAL_MS * 1000; // max time to wait in MICROSECONDS !
        activity = select( max_sd+1, &readfds , NULL , NULL , &timeout);
        // > The select function returns the total number of socket handles
        // > that are ready and contained in the fd_set structures,
        // > zero if the time limit expired, or SOCKET_ERROR
        // > if an error occurred. If the return value is SOCKET_ERROR,
        // > WSAGetLastError can be used to retrieve a specific error code. ...
        if( activity == 0 )  // > "zero if the time limit expired"...
         { // .. which means NOTHING HAPPENED on the socket,
           //    so don't waste time checking FD_ISSET(),
           //    just loop around and sleep for another 20 ms in select(),
           //    unless this client needs to *SEND* unsolicited data:
           if( ! pCwNet->fSimulateBadConnNow )
            { CwNet_OnPoll( pCwNet, &pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX] ); // here: called from ClientThread()
              //  '--> May deposit 'unsolicited' data in pCwNet->bTxBuffer !
            }
           if( pCwNet->nBytesInTxBuffer > 0 ) // send "unsolicited data" (not "Nagle friendly") ?
            { if( send( s, (const char *)pCwNet->bTxBuffer, pCwNet->nBytesInTxBuffer, 0)
                                           != pCwNet->nBytesInTxBuffer )
               { pCwNet->pszLastError = "ClientThread failed to send unsolicited data";
               }
              else // successfully SENT something, so COUNT this (for statistics):
               { pCwNet->dwNumBytesSent  += pCwNet->nBytesInTxBuffer;
                 pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX].dwNumBytesSent += pCwNet->nBytesInTxBuffer;
                 // CwServer_FeedActivityWatchdog( &pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX] ); // feed the dog ?
#               if(0) // Removed 2025-01, because with all those AUDIO- and SPECTRUM data, too much for the 'Debug' tab !
                 if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                  { INET_DumpTraffic_HexOrASCII( sz1kDump,500, pCwNet->bTxBuffer, pCwNet->nBytesInTxBuffer );
                    ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX %s", sz1kDump );
                  }
#               endif // removed the 'hex dump of EVERYTHING' in 2025-01
                 pCwNet->nBytesInTxBuffer = 0;
               }
            }
         } // end if( activity == 0 ), i.e. "nothing received but no error"
        else if (activity < 0) // > "SOCKET_ERROR (-1) if an error occurred"...
         { // select() failed.  "This is a permanent error" ... close the socket, try again later
           ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Client: select() failed; closing socket, try later." );
           CwNet_CloseClientSocketAndStartWaitingForReconnect( pCwNet, &s );
              //     '--> CWNET_CONN_STATE_WAIT_RECONN
         }
        else // neither "timeout" in select() or a "complete fail" -> SOMETHING must have happened !
        if (FD_ISSET( s, &readfds)) // Mr. select() promised that Mr. recv() won't block...
         {
           valread = recv( s, (char *)pCwNet->bRxBuffer, CWNET_SOCKET_RX_BUFFER_SIZE, 0);
           if( valread == SOCKET_ERROR)
            {
              int error_code = WSAGetLastError();
              if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
               { ShowError( ERROR_CLASS_ERROR | SHOW_ERROR_TIMESTAMP, "Client: recv() failed (%s)",
                            INET_WinsockErrorCodeToString( error_code ) );
               }
              if(error_code == WSAECONNRESET)
               { // Someone pulled the plug, not necessarily the REMOTE SERVER ..
                 // Close the socket, restart the "waiting timer" ..
                 CwNet_CloseClientSocketAndStartWaitingForReconnect( pCwNet, &s );
                 //     '--> CWNET_CONN_STATE_WAIT_RECONN
               }
              else // revc() failed for some other reason
               {   // also pull the plug, wait, then try to plug it in again ?
                 CwNet_CloseClientSocketAndStartWaitingForReconnect( pCwNet, &s );
               }
            } // end if < recv() -> SOCKET_ERROR > ?
           if ( valread == 0)
            {
              // Somebody disconnected ...
              if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
               { ShowError( ERROR_CLASS_ERROR | SHOW_ERROR_TIMESTAMP, "Client: recv() returned 0 (disconnected?)" );
               }
              CwNet_CloseClientSocketAndStartWaitingForReconnect( pCwNet, &s );
            }
           else // something received ->
            { pCwNet->nBytesInRxBuffer = valread;
              if( pCwNet->nBytesInRxBuffer > pCwNet->iPeakRxBufferUsage )
               {  pCwNet->iPeakRxBufferUsage = pCwNet->nBytesInRxBuffer;
               }
              pCwNet->dwNumBytesRcvd += pCwNet->nBytesInRxBuffer; // here: inc'd in ClientThread()
              CwNet_OnReceive( pCwNet, &pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX], pCwNet->bRxBuffer, pCwNet->nBytesInRxBuffer );
              //  '--> May already deposit some data in pCwNet->bTxBuffer !
              if( ! pCwNet->fSimulateBadConnNow )
               { CwNet_OnPoll( pCwNet, &pCwNet->Client[CWNET_LOCAL_CLIENT_INDEX] ); // here: also called from ClientThread()
                 //  '--> May deposit MORE data in pCwNet->bTxBuffer !
               }
              if( pCwNet->nBytesInTxBuffer > 0 ) // send a RESPONSE (here: "Nagle friendly") ?
               { if( send( s, (const char *)pCwNet->bTxBuffer, pCwNet->nBytesInTxBuffer, 0)
                                              != pCwNet->nBytesInTxBuffer )
                  { pCwNet->pszLastError = "ClientThread failed to send response";
                  }
                 else
                  { pCwNet->dwNumBytesSent += pCwNet->nBytesInTxBuffer;

#                  if(0) // Removed 2025-01, because with all those AUDIO- and SPECTRUM data, too much for the 'Debug' tab !
                    if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_SHOW_NETWORK_TRAFFIC ) // beware, a "CPU hog"..
                     { INET_DumpTraffic_HexOrASCII( sz1kDump,500, pCwNet->bTxBuffer, pCwNet->nBytesInTxBuffer );
                       ShowError( ERROR_CLASS_TX_TRAFFIC | SHOW_ERROR_TIMESTAMP, "TX %s", sz1kDump );
                     }
#                  endif // removed the 'hex dump of EVERYTHING' in 2025-01
                    pCwNet->nBytesInTxBuffer = 0;
                  }
               }
            }
         } // end if (FD_ISSET( s , &readfds))
      } // end if < 'Actively communicate' > ?
     else // Because there's no CONNECTED socket, or fSimulateBadConnNow is SET,
      {   //         can't "sleep" in select(),
          // so give the CPU to anyone else by "sleeping" in Sleep() .
          // This is an ugly hack and there are more elegant methods,
          // but any attempt to "wait for connect() using select()" failed
          // (WB didn't want to mess around with "jocktellsocket FIONBIO" & Co)
          Sleep( CWNET_SOCKET_POLLING_INTERVAL_MS );  // holy crap ... but KISS
      }

   }   // end while < client-thread shall run >

  if( s != INVALID_SOCKET )
   { closesocket(s);
   }
  if( pCwNet->cfg.iDiagnosticFlags & CWNET_DIAG_FLAGS_VERBOSE )
   { ShowError( ERROR_CLASS_INFO | SHOW_ERROR_TIMESTAMP, "Client: Disconnected and thread terminated" );
   }
  return 0;
} // end ClientThread()


//---------------------------------------------------------------------------
// Helper functions for commands (ASCII strings) with a 'Hamlib'-like syntax
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
long CwNet_ParseIntFromHamlibCmdArgs( const char *pszSrc )
{ // To Do: skip all those ever-changing "VFO names" (which we don't need) ?
  return SL_atoi( pszSrc );
} // end CwNet_ParseIntFromHamlibCmdArgs()

//---------------------------------------------------------------------------
// Functions invoked via FUNCTION POINTER after recognizing a Hamlib command.
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnGetFreq(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
               const char *pszCmdArgs, char *pszResp, int iMaxRespLength  )
{
  // From www.mankier.com/1/rigctl (same at hamlib.sourceforge.net/html/rigctld.1.html) :
  // > f, get_freq
  // >  Get 'Frequency', in Hz.
  // >  Returns an integer value and the VFO hamlib thinks is active.  (???)
  // >  Note that some rigs (e.g. all Icoms) cannot track current VFO
  // >  so hamlib can get out of sync with the rig if the user presses
  // >  rig buttons like the VFO.
  // Seen in the Wireshark dump (between WSJT-X and wfview's rigctld-emulation):
  //   Command:   "f\n"          // short form of "what's the frequency" ?
  //   Response:  "144300000\n"  // here is our frequency in Hertz  (note the absence of a bloody "VFO hamblib thinks is active" - it's JUST A FREQUENCY, full stop.)
  return CwNet_Rigctld_PrintToResponse( pCwNet, pClient, pszResp, iMaxRespLength,
        "%ld\n", (long)pCwNet->RigControl.dblVfoFrequency );
} // end CwNet_Rigctld_OnGetFreq()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnSetFreq(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  // What to send in response to a SET command (in 'rigctld') ?
  // Found this in https://hamlib.sourceforge.net/html/rigctld.1.html :
  // > A one line response will be sent as a reply to set commands, "RPRT x\n"
  // > where x is the Hamlib error code with '0' indicating success of the command.
  // Because this is the
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnSetFreq()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnGetMode(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnGetMode()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnSetMode(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnSetMode()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnGetVFO( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  // From www.mankier.com/1/rigctl (same at hamlib.sourceforge.net/html/rigctld.1.html) :
  // > v, get_vfo
  // >  Get current 'VFO'.
  // >  Returns VFO as a token as in set_vfo above.
  // Seen in the Wireshark dump (between WSJT-X and wfview's rigctld-emulation):
  //   Command:   "v\n"   (short form of "get_vfo")
  //   Response:  "VFOA"  ("the current VFO is VFO A" or something in that style)
  return CwNet_Rigctld_PrintToResponse( pCwNet, pClient, pszResp, iMaxRespLength, "VFOA\n" );
} // end CwNet_Rigctld_OnGetVFO()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnSetVFO( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnSetVFO()


//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnGetRIT(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnGetRIT()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnSetRIT(T_CwNet *pCwNet,
              T_CwNetClient *pClient,
              T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnSetRIT()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnGetSplitVFO(T_CwNet *pCwNet,
              T_CwNetClient *pClient,
              T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  // > s, get_split_vfo
  // >  Get 'Split' mode.
  // >  Split is either 0 = Normal   or   1 = Split.  (an IC-9700 knows other split modes; see RigControl.c : cmd = 0x0F ! )
  // >  Get 'TX VFO'.
  // >  TX VFO is a token as in set_split_vfo .
  // Seen in the Wireshark dump (between WSJT-X and wfview's rigctld-emulation):
  //   Command:   "s\n" (short form of "get_split_vfo")
  //   Response:  "0"   ("the current VFO is NOT in split mode" or something in that style)
  //   (2nd line) "VFOB" (if the current VFO was using split mode, the TX VFO would be "VFOB")
  //
  long i32 = RigCtrl_GetParamByPN_Int( &pCwNet->RigControl, RIGCTRL_PN_SPLIT_MODE);
    // 2024-06-30 : Got here with i32 = 0x80000001 = -2147483647 = RIGCTRL_NOVALUE_INT,
    //              because 'split mode' wasn't one of the parameters polled
    //              from the radio after the initial connect. The same problem
    //              would occur with other 'exotic' parameters that only
    //              the hamlib clients in some 3rd party software would use.
    //  Fixed by reading (or at least TRYING TO read) those parameters at least
    //  once, in Remote_CW_Keyer/RigControl.c : RigCtrl_Handler() /
  return CwNet_Rigctld_PrintToResponse( pCwNet, pClient, pszResp, iMaxRespLength, "%d\nVFOB\n", (int)i32 );
} // end CwNet_Rigctld_OnGetSplitVFO()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnSetSplitVFO(T_CwNet *pCwNet,
              T_CwNetClient *pClient,
              T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{ // > S, set_split_vfo 'Split' 'TX VFO'
  // >  Set 'Split' mode.
  // >  Split is either ’0’ = Normal or ’1’ = Split.
  // >
  // >  Set 'TX VFO'.
  // >  TX VFO is a token: ’VFOA’, ’VFOB’, ’VFOC’, ’currVFO’,
  // >                     ’VFO’, ’MEM’, ’Main’, ’Sub’, ’TX’, ’RX’ .
  int iSplitMode = SL_ParseInteger( &pszCmdArgs );
  RigCtrl_SetParamByPN_Int( &pCwNet->RigControl, RIGCTRL_PN_SPLIT_MODE, iSplitMode );
  // The 2nd argument, "TX VFO", is ignored here.
  return HAMLIB_RESULT_NO_ERROR;

} // end CwNet_Rigctld_OnSetSplitVFO()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnGetXIT( T_CwNet *pCwNet,
              T_CwNetClient *pClient,
              T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnGetXIT()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnSetXIT( T_CwNet *pCwNet,
              T_CwNetClient *pClient,
              T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnSetXIT()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnGetPTT( T_CwNet *pCwNet,  // implements "get_ptt"
              T_CwNetClient *pClient,
              T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  // > "t, get_ptt"        (from www.mankier.com/1/rigctl anno 2024)
  // >   Get 'PTT' status.
  // >   Returns PTT as a value in set_ptt .
  long i32 = RigCtrl_GetParamByPN_Int( &pCwNet->RigControl, RIGCTRL_PN_TRANSMITTING);
    // '-->  0=receiving, 1=transmitting, or RIGCTRL_NOVALUE_INT when NOT AVAILABLE
  if( i32==RIGCTRL_NOVALUE_INT ) // if PTT status isn't *available*, indicate RECEPTION
   {  i32 = 0;
   }
  return CwNet_Rigctld_PrintToResponse( pCwNet, pClient, pszResp, iMaxRespLength, "%d\n", (int)i32 );
  // Note: In Hamlib, the 'PTT' status value may be
  //       0 = RX,  1 =  TX,  2 = "TX mic", or 3 = "TX data".
  //       HERE, we don't care if we're 'transmitting a microphone',
  //                                    'transmitting data',
  //                                    or transmitting Morse code.
} // end CwNet_Rigctld_OnGetPTT()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnSetPTT( T_CwNet *pCwNet,  // implements "set_ptt"
              T_CwNetClient *pClient,
              T_CwNet_CmdTabEntry *pCmdInfo,
              const char* pszCmdArgs, char *pszResp, int iMaxRespLength )
{
  // > "T, set_ptt 'PTT'"  (from www.mankier.com/1/rigctl anno 2024)
  // >   Set 'PTT'.
  // >   PTT is a value: '0' (RX), '1' (TX), '2' (TX mic), or '3' (TX data).
  // (Fortunately, this 'rigctld' command doesn't torture us with a lot of
  //  funny 'VFO names' seen elsewhere.
  //  Leave those apostrophes away, they don't really exist in the strings.)
  // This is the FIRST COMMAND actually implemented here (in CwNet.c) .
  // It only makes sense in the Remote CW Keyer running "as server",
  //                     controlling a real radio via RigControl.c .
  int iTransmitReqst;
  if( pClient->iPermissions & CWNET_PERMISSION_TRANSMIT )
   { // Permission granted for the current user, so pass this on to the real rig:
     iTransmitReqst = CwNet_ParseIntFromHamlibCmdArgs(pszCmdArgs);
     if( iTransmitReqst != RIGCTRL_NOVALUE_INT )
      { // ex: RigCtrl_SetTransmitRequest( &pCwNet->RigControl, iTransmitReqst ); // <- now exclusively called from KeyerThread.c !
        pCwNet->iSetPTTFromNetwork = iTransmitReqst; // <- flag polled in KeyerThread.c : KeyerThread(), like the local digital "PTT" input
        //
        return HAMLIB_RESULT_NO_ERROR;
      }
     else
      { return HAMLIB_RESULT_ARG_OUT_OF_DOM;
      }
   }
  else // No "permission to transmit" for the client that wants to control the PTT ->
   { // Guessed which of the circa 20 "Hamlib error codes" defined in
     //     C:\cbproj\Remote_CW_Keyer\HamlibResultCodes.h to send now.
     // Of course there is nothing like e.g. "HAMLIB_RESULT_NO_PERMISSION".
     // Maybe "HAMLIB_RESULT_SECURITY_ERROR" (-20)
     //   or  "HAMLIB_RESULT_REJECTED_BY_RIG" (-9)
     //   or  "HAMLIB_RESULT_NOT_AVAILABLE"  (-11) ?
     // Tossed the dice, made my choice .. and decided on "Function not available",
     // because it's not "the rig that rejected controlling the PTT" but this software.
     return HAMLIB_RESULT_NOT_AVAILABLE; // here: due to insufficient PERMISSIONS
   }
} // end CwNet_Rigctld_OnSetPTT()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnDumpState(T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
  // Does NOT implement Hamlib's "dump_state" command, at least not HERE in CwNet.c !
  // From hamlib.sourceforge.net/html/rigctld.1.html :
  // > Return certain state information about the radio backend.
  // A-ha. But what's the format we're expected to send ? DOCUMENTATION ? ? ?
  // We can only GUESS the meanings of all lines in the response from e.g.
  //     Hamlib-4.5.5/tests/rigctl_parse.c, but it's too obfuscated.
{
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnDumpState()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnCheckVFO( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
  // When tested with WSJT-X as remote client, "chk_vfo" was the first received
  // command. Purpose ( from hamlib.sourceforge.net/html/rigctld.1.html ) :
  // > Returns "CHKVFO 1\n" (single line only) if rigctld was invoked
  // > with the -o/--vfo option and "CHKVFO 0\n" if not.
  // > When in VFO mode the client will need to pass 'VFO' as the first parameter
  // > to set or get commands. VFO is one of the strings defined in set_vfo above.
  // Hmm. Much too complicated for the intended purpose.
{
  return CwNet_Rigctld_PrintToResponse( pCwNet, pClient, pszResp, iMaxRespLength, "CHKVFO %d\n", (int)0 );
} // end CwNet_Rigctld_OnCheckVFO()

//---------------------------------------------------------------------------
static int CwNet_Rigctld_OnQuit( T_CwNet *pCwNet, T_CwNetClient *pClient, T_CwNet_CmdTabEntry *pCmdInfo,
              const char *pszCmdArgs, char *pszResp, int iMaxRespLength )
  // Not sure if the long command form is really "exit rigctl"
  //     as listed at www.mankier.com/1/rigctl under "rigctl Command".
  //     No other command uses a space character (e.g. "set_freq", with underscore).
  // NOT listed as a valid command at hamlib.sourceforge.net/html/rigctld.1.html
  //     (under "COMMANDS") at all.
  // WSJT-X sent this command in the short form, so we probably need it.
  // > Q|q, exit rigctl
  // >  Exit rigctl in interactive mode.
  // >  When rigctl is controlling the rig directly, will close
  // >  the rig backend and port.  When rigctl is connected to rigctld
  // >  (radio model 2), the TCP/IP connection to rigctld is closed and
  // >  rigctld remains running, available for another TCP/IP network connection.
{
  pClient->fDisconnect = TRUE;
  return HAMLIB_RESULT_NO_ERROR;
} // end CwNet_Rigctld_OnQuit()



/* EOF < CwNet.c > */










