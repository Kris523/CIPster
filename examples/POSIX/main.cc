/*******************************************************************************
 * Copyright (c) 2009, Rockwell Automation, Inc.
 * All rights reserved.
 *
 ******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "networkhandler.h"
#include "opener_api.h"

extern int newfd;

// ****************************************************************************
/** @brief Signal handler function for ending stack execution
 *
 * @param signal the signal we received
 */
void LeaveStack( int signal );

// ***************************************************************************
/** @brief Flag indicating if the stack should end its execution
 */
int g_end_stack = 0;

// ****************************************************************************
int main( int argc, char* argv[] )
{
    EipUint8 my_mac_address[6];
    EipUint16 unique_connection_id;

    if( argc != 12 )
    {
        printf( "Wrong number of command line parameters! %d instead of 12\n", argc );
        printf( "The correct command line parameters are:\n" );
        printf( "%s ipaddress subnetmask gateway domainname hostaddress macaddress\n", argv[0] );
        printf( "e.g.\n" );
        printf( "    %s 192.168.0.2 255.255.255.0 192.168.0.1 test.com testdevice 00 15 C5 BF D0 87\n", argv[0] );
        exit( 0 );
    }
    else
    {
        // fetch Internet address info from the platform
        ConfigureNetworkInterface( argv[1], argv[2], argv[3] );
        ConfigureDomainName( argv[4] );
        ConfigureHostName( argv[5] );

        my_mac_address[0]   = (EipUint8) strtoul( argv[6], NULL, 16 );
        my_mac_address[1]   = (EipUint8) strtoul( argv[7], NULL, 16 );
        my_mac_address[2]   = (EipUint8) strtoul( argv[8], NULL, 16 );
        my_mac_address[3]   = (EipUint8) strtoul( argv[9], NULL, 16 );
        my_mac_address[4]   = (EipUint8) strtoul( argv[10], NULL, 16 );
        my_mac_address[5]   = (EipUint8) strtoul( argv[11], NULL, 16 );
        ConfigureMacAddress( my_mac_address );
    }

    // for a real device the serial number should be unique per device
    SetDeviceSerialNumber( 123456789 );

    /* nUniqueConnectionID should be sufficiently random or incremented and stored
     *  in non-volatile memory each time the device boots.
     */
    unique_connection_id = rand();

    // Setup the CIP Layer
    CipStackInit( unique_connection_id );

    // Setup Network Handles
    if( kEipStatusOk == NetworkHandlerInitialize() )
    {
        g_end_stack = 0;

#ifndef _WIN32
        // register for closing signals so that we can trigger the stack to end
        signal( SIGHUP, LeaveStack );
        signal( SIGINT, LeaveStack );
#endif

        printf( "running...\n" );

        // The event loop. Put other processing you need done continually in here
        while( 1 != g_end_stack )
        {
            if( kEipStatusOk != NetworkHandlerProcessOnce() )
            {
                break;
            }
        }

        printf( "\ncleaning up and ending...\n" );

        // clean up network state
        NetworkHandlerFinish();
    }

    // close remaining sessions and connections, cleanup used data
    ShutdownCipStack();

    return -1;
}


void LeaveStack( int signal )
{
    (void) signal;      // kill unused parameter warning

    CIPSTER_TRACE_STATE( "got signal\n" );

    g_end_stack = 1;
}
