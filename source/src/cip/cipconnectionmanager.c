/*******************************************************************************
 * Copyright (c) 2009, Rockwell Automation, Inc.
 * All rights reserved.
 *
 ******************************************************************************/
#include <string.h>

#include "cipconnectionmanager.h"

#include "opener_user_conf.h"
#include "cipcommon.h"
#include "cipmessagerouter.h"
#include "ciperror.h"
#include "endianconv.h"
#include "opener_api.h"
#include "encap.h"
#include "cipidentity.h"
#include "trace.h"
#include "cipclass3connection.h"
#include "cipioconnection.h"
#include "cipassembly.h"
#include "cpf.h"
#include "appcontype.h"
#include "encap.h"

// values needed from the CIP identity object
extern EipUint16    vendor_id_;
extern EipUint16    device_type_;
extern EipUint16    product_code_;
extern CipRevision  revision_;

#define CIP_CONN_TYPE_MASK 0x6000           //*< Bit mask filter on bit 13 & 14

bool table_init = true;

const int g_kForwardOpenHeaderLength = 36;  //*< the length in bytes of the forward open command specific data till the start of the connection path (including con path size)

/// @brief Compares the logical path on equality
#define EQLOGICALPATH( x, y ) ( ( (x) & 0xfc )==(y) )

#define NUM_CONNECTABLE_OBJECTS     (2 + OPENER_CIP_NUM_APPLICATION_SPECIFIC_CONNECTABLE_OBJECTS)

struct ConnectionManagementHandling
{
    EipUint32               class_id;
    OpenConnectionFunction  open_connection_function;
};

/** List holding information on the object classes and open/close function
 * pointers to which connections may be established.
 */
static ConnectionManagementHandling g_astConnMgmList[NUM_CONNECTABLE_OBJECTS];


/// List holding all currently active connections
/*@null@*/ CipConn* g_active_connection_list = NULL;


/// buffer connection object needed for forward open
static CipConn s_dummy_conn;

/// @brief Holds the connection ID's "incarnation ID" in the upper 16 bits
EipUint32 g_incarnation_id;

// private functions
EipStatus ForwardOpen( CipInstance* instance,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response );

EipStatus ForwardClose( CipInstance* instance,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response );

EipStatus GetConnectionOwner( CipInstance* instance,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response );

EipStatus AssembleForwardOpenResponse( CipConn* cip_conn,
        CipMessageRouterResponse* response, EipUint8 general_status,
        EipUint16 extended_status );

EipStatus AssembleForwardCloseResponse( EipUint16 connection_serial_number,
        EipUint16 originatior_vendor_id,
        EipUint32 originator_serial_number,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response,
        EipUint16 extended_error_code );

void AddNullAddressItem( CipCommonPacketFormatData* cpfd );

/** @brief check if the data given in the connection object match with an already established connection
 *
 * The comparison is done according to the definitions in the CIP specification Section 3-5.5.2:
 * The following elements have to be equal: Vendor ID, Connection Serial Number, Originator Serial Number
 * @param cip_conn connection object containing the comparison elements from the forward open request
 * @return
 *    - NULL if no equal established connection exists
 *    - pointer to the equal connection object
 */
CipConn* CheckForExistingConnection( CipConn* cip_conn );

/** @brief Compare the electronic key received with a forward open request with the device's data.
 *
 * @param key_format format identifier given in the forward open request
 * @param key_data pointer to the electronic key data received in the forward open request
 * @param extended_status the extended error code in case an error happened
 * @return general status on the establishment
 *    - EIP_OK ... on success
 *    - On an error the general status code to be put into the response
 */
EipStatus CheckElectronicKeyData( EipUint8 key_format, CipKeyData* key_data,
        EipUint16* extended_status );

/** @brief Parse the connection path of a forward open request
 *
 * This function will take the connection object and the received data stream and parse the connection path.
 * @param cip_conn pointer to the connection object structure for which the connection should
 *                      be established
 * @param request pointer to the received request structure. The position of the data stream pointer has to be at the connection length entry
 * @param extended_status the extended error code in case an error happened
 * @return general status on the establishment
 *    - EIP_OK ... on success
 *    - On an error the general status code to be put into the response
 */
EipUint8 ParseConnectionPath( CipConn* cip_conn,
        CipMessageRouterRequest* request,
        EipUint16* extended_error );

ConnectionManagementHandling* GetConnMgmEntry( EipUint32 class_id );

void InitializeConnectionManagerData();

/** @brief gets the padded logical path TODO: enhance documentation
 * @param logical_path_segment TheLogical Path Segment
 *
 */
unsigned GetPaddedLogicalPath( unsigned char** logical_path_segment )
{
    unsigned padded_logical_path = *(*logical_path_segment)++;

    if( (padded_logical_path & 3) == 0 )
    {
        padded_logical_path = *(*logical_path_segment)++;
    }
    else if( (padded_logical_path & 3) == 1 )
    {
        (*logical_path_segment)++; // skip pad
        padded_logical_path = *(*logical_path_segment)++;
        padded_logical_path |= *(*logical_path_segment)++ << 8;
    }
    else
    {
        OPENER_TRACE_ERR( "illegal logical path segment\n" );
    }

    return padded_logical_path;
}


/** @brief Generate a new connection Id utilizing the Incarnation Id as
 * described in the EIP specs.
 *
 * A unique connectionID is formed from the boot-time-specified "incarnation ID"
 * and the per-new-connection-incremented connection number/counter.
 * @return new connection id
 */
EipUint32 GetConnectionId()
{
    static EipUint32 connection_id = 18;

    connection_id++;
    return g_incarnation_id | (connection_id & 0x0000FFFF);
}


EipStatus ConnectionManagerInit( EipUint16 unique_connection_id )
{
    InitializeConnectionManagerData();

    CipClass* connection_manager = CreateCipClass(
            g_kCipConnectionManagerClassCode,   // class ID
            0xC6,                               // class getAttributeAll mask
            0xffffffff,                         // instance getAttributeAll mask
            1,                                  // no. instances
            "connection manager",               // class name
            1                                   // revision
            );

    if( !connection_manager )
        return kEipStatusError;

    InsertService( connection_manager, kForwardOpen, &ForwardOpen, "ForwardOpen" );

    InsertService( connection_manager, kForwardClose, &ForwardClose, "ForwardClose" );

    InsertService( connection_manager, kGetConnectionOwner, &GetConnectionOwner, "GetConnectionOwner" );

    g_incarnation_id = ( (EipUint32) unique_connection_id ) << 16;

    AddConnectableObject( kCipMessageRouterClassCode, EstablishClass3Connection );
    AddConnectableObject( kCipAssemblyClassCode, EstablishIoConnction );

    return kEipStatusOk;
}


EipStatus HandleReceivedConnectedData( EipUint8* data, int data_length,
        struct sockaddr_in* from_address )
{
    OPENER_TRACE_INFO( "%s:\n", __func__ );

    if( ( CreateCommonPacketFormatStructure( data, data_length,
                  &g_common_packet_format_data_item ) ) == kEipStatusError )
    {
        return kEipStatusError;
    }
    else
    {
        // Check if connected address item or sequenced address item  received,
        // otherwise it is no connected message and should not be here.
        if( (g_common_packet_format_data_item.address_item.type_id == kCipItemIdConnectionAddress)
         || (g_common_packet_format_data_item.address_item.type_id == kCipItemIdSequencedAddressItem) )
        {
            // found connected address item or found sequenced address item
            // -> for now the sequence number will be ignored

            if( g_common_packet_format_data_item.data_item.type_id
                == kCipItemIdConnectedDataItem ) // connected data item received
            {
                CipConn* conn = GetConnectedObject(
                        g_common_packet_format_data_item.address_item.data
                        .connection_identifier );

                if( !conn )
                    return kEipStatusError;

                // only handle the data if it is coming from the originator
                if( conn->originator_address.sin_addr.s_addr == from_address->sin_addr.s_addr )
                {
                    if( SEQ_GT32(
                                g_common_packet_format_data_item.address_item.data.sequence_number,
                                conn->eip_level_sequence_count_consuming ) )
                    {
                        // reset the watchdog timer
                        conn->inactivity_watchdog_timer =
                            (conn->o_to_t_requested_packet_interval / 1000) << (2 + conn->connection_timeout_multiplier);

                        OPENER_TRACE_INFO( "%s: reset inactivity_watchdog_timer:%u\n",
                            __func__,
                            conn->inactivity_watchdog_timer );

                        // only inform assembly object if the sequence counter is greater or equal
                        conn->eip_level_sequence_count_consuming =
                            g_common_packet_format_data_item.address_item.data.sequence_number;

                        if( conn->connection_receive_data_function )
                        {
                            return conn->connection_receive_data_function(
                                    conn,
                                    g_common_packet_format_data_item.data_item.data,
                                    g_common_packet_format_data_item.data_item.length );
                        }
                    }
                }
                else
                {
                    OPENER_TRACE_WARN(
                            "Connected Message Data Received with wrong address information\n" );
                }
            }
        }
    }

    return kEipStatusOk;
}


/*   @brief Check if resources for new connection available, generate ForwardOpen Reply message.
 *      instance	pointer to CIP object instance
 *      request		pointer to Message Router Request.
 *      response		pointer to Message Router Response.
 *      @return >0 .. success, 0 .. no reply to send back
 *          -1 .. error
 */
EipStatus ForwardOpen( CipInstance* instance,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response )
{
    EipUint16 connection_status = kConnectionManagerStatusCodeSuccess;
    ConnectionManagementHandling* connection_management_entry;

    (void) instance;        //suppress compiler warning

    //first check if we have already a connection with the given params
    s_dummy_conn.priority_timetick = *request->data++;
    s_dummy_conn.timeout_ticks = *request->data++;

    // O_to_T Conn ID
    s_dummy_conn.consumed_connection_id = GetDintFromMessage( &request->data );

    // T_to_O Conn ID
    s_dummy_conn.produced_connection_id = GetDintFromMessage( &request->data );

    s_dummy_conn.connection_serial_number = GetIntFromMessage( &request->data );
    s_dummy_conn.originator_vendor_id = GetIntFromMessage( &request->data );
    s_dummy_conn.originator_serial_number = GetDintFromMessage( &request->data );

    if( ( NULL != CheckForExistingConnection( &s_dummy_conn ) ) )
    {
        /* TODO this test is  incorrect, see CIP spec 3-5.5.2 re: duplicate forward open
         *  it should probably be testing the connection type fields
         *  TODO think on how a reconfiguration request could be handled correctly */
        if( (0 == s_dummy_conn.consumed_connection_id)
            && (0 == s_dummy_conn.produced_connection_id) )
        {
            //TODO implement reconfiguration of connection

            OPENER_TRACE_ERR(
                    "this looks like a duplicate forward open -- I can't handle this yet, sending a CIP_CON_MGR_ERROR_CONNECTION_IN_USE response\n" );
        }

        return AssembleForwardOpenResponse(
                &s_dummy_conn, response,
                kCipErrorConnectionFailure,
                kConnectionManagerStatusCodeErrorConnectionInUse );
    }

    /* keep it to none existent till the setup is done this eases error handling and
     * the state changes within the forward open request can not be detected from
     * the application or from outside (reason we are single threaded)*/
    s_dummy_conn.state = kConnectionStateNonExistent;
    s_dummy_conn.sequence_count_producing = 0; // set the sequence count to zero

    s_dummy_conn.connection_timeout_multiplier = *request->data++;
    request->data += 3; // reserved

    // the requested packet interval parameter needs to be a multiple of TIMERTICK from the header file
    OPENER_TRACE_INFO(
            "ForwardOpen: ConConnID %" PRIu32 ", ProdConnID %" PRIu32 ", ConnSerNo %u\n",
            s_dummy_conn.consumed_connection_id,
            s_dummy_conn.produced_connection_id,
            s_dummy_conn.connection_serial_number );

    s_dummy_conn.o_to_t_requested_packet_interval = GetDintFromMessage( &request->data );

    s_dummy_conn.o_to_t_network_connection_parameter = GetIntFromMessage( &request->data );
    s_dummy_conn.t_to_o_requested_packet_interval    = GetDintFromMessage( &request->data );

    EipUint32 temp = s_dummy_conn.t_to_o_requested_packet_interval
                     % (kOpenerTimerTickInMilliSeconds * 1000);

    if( temp > 0 )
    {
        s_dummy_conn.t_to_o_requested_packet_interval =
            (EipUint32) ( s_dummy_conn.t_to_o_requested_packet_interval
                          / (kOpenerTimerTickInMilliSeconds * 1000) )
            * (kOpenerTimerTickInMilliSeconds * 1000)
            + (kOpenerTimerTickInMilliSeconds * 1000);
    }

    s_dummy_conn.t_to_o_network_connection_parameter = GetIntFromMessage( &request->data );

    //check if Network connection parameters are ok
    if( CIP_CONN_TYPE_MASK == (s_dummy_conn.o_to_t_network_connection_parameter & CIP_CONN_TYPE_MASK) )
    {
        return AssembleForwardOpenResponse(
                &s_dummy_conn, response,
                kCipErrorConnectionFailure,
                kConnectionManagerStatusCodeErrorInvalidOToTConnectionType );
    }

    if( CIP_CONN_TYPE_MASK == (s_dummy_conn.t_to_o_network_connection_parameter & CIP_CONN_TYPE_MASK) )
    {
        return AssembleForwardOpenResponse(
                &s_dummy_conn, response,
                kCipErrorConnectionFailure,
                kConnectionManagerStatusCodeErrorInvalidTToOConnectionType );
    }

    s_dummy_conn.transport_type_class_trigger = *request->data++;

    //check if the trigger type value is ok
    if( 0x40 & s_dummy_conn.transport_type_class_trigger )
    {
        return AssembleForwardOpenResponse(
                &s_dummy_conn, response,
                kCipErrorConnectionFailure,
                kConnectionManagerStatusCodeErrorTransportTriggerNotSupported );
    }

    temp = ParseConnectionPath( &s_dummy_conn, request, &connection_status );

    if( kEipStatusOk != temp )
    {
        return AssembleForwardOpenResponse( &s_dummy_conn,
                response, temp,
                connection_status );
    }

    // parsing is now finished all data is available and check now establish the connection
    connection_management_entry = GetConnMgmEntry( s_dummy_conn.connection_path.class_id );
    if( connection_management_entry )
    {
        temp = connection_management_entry->open_connection_function(
                &s_dummy_conn, &connection_status );

        OPENER_TRACE_INFO( "open_connection_function, temp = %d\n", temp );
    }
    else
    {
        OPENER_TRACE_INFO( "%s: GetConnMgmEntry returned NULL\n", __func__ );
        temp = kEipStatusError;
        connection_status = kConnectionManagerStatusCodeInconsistentApplicationPathCombo;
    }

    if( kEipStatusOk != temp )
    {
        OPENER_TRACE_INFO( "connection manager: connect failed. temp:%d\n", temp );

        // in case of error the dummy objects holds all necessary information
        return AssembleForwardOpenResponse( &s_dummy_conn,
                response, temp,
                connection_status );
    }
    else
    {
        OPENER_TRACE_INFO( "connection manager: connect succeeded\n" );

        // in case of success the g_pstActiveConnectionList points to the new connection
        return AssembleForwardOpenResponse( g_active_connection_list,
                response,
                kCipErrorSuccess, 0 );
    }
}


void GeneralConnectionConfiguration( CipConn* conn )
{
    if( kRoutingTypePointToPointConnection == (conn->o_to_t_network_connection_parameter & kRoutingTypePointToPointConnection) )
    {
        /* if we have a point to point connection for the O to T direction
         * the target shall choose the connection ID.
         */
        conn->consumed_connection_id = GetConnectionId();
    }

    if( kRoutingTypeMulticastConnection == (conn->t_to_o_network_connection_parameter & kRoutingTypeMulticastConnection) )
    {
        /* if we have a multi-cast connection for the T to O direction the
         * target shall choose the connection ID.
         */
        conn->produced_connection_id = GetConnectionId();
    }

    conn->eip_level_sequence_count_producing = 0;
    conn->sequence_count_producing = 0;
    conn->eip_level_sequence_count_consuming = 0;
    conn->sequence_count_consuming = 0;

    conn->watchdog_timeout_action = kWatchdogTimeoutActionAutoDelete;  // the default for all connections on EIP

    conn->expected_packet_rate = 0;                                    // default value

    if( !(conn->transport_type_class_trigger & 0x80) )  // Client Type Connection requested
    {
        conn->expected_packet_rate = (EipUint16) ( (conn->t_to_o_requested_packet_interval) / 1000 );

        /* As soon as we are ready we should produce the connection. With the 0
         * here we will produce with the next timer tick
         * which should be sufficient.
         */
        conn->transmission_trigger_timer = 0;
    }
    else
    {
        // Server Type Connection requested
        conn->expected_packet_rate = (EipUint16) ( (conn->o_to_t_requested_packet_interval) / 1000 );
    }

    conn->production_inhibit_timer = conn->production_inhibit_time = 0;

    // setup the preconsuption timer: max(ConnectionTimeoutMultiplier * EpectetedPacketRate, 10s)
    conn->inactivity_watchdog_timer =
        ( ( ( (conn->o_to_t_requested_packet_interval) / 1000 )
            << (2 + conn->connection_timeout_multiplier) ) > 10000 ) ?
        ( ( (conn->o_to_t_requested_packet_interval) / 1000 )
          << (2 + conn->connection_timeout_multiplier) ) :
        10000;

    OPENER_TRACE_INFO( "%s: inactivity_watchdog_timer:%u\n", __func__,
            conn->inactivity_watchdog_timer );

    conn->consumed_connection_size = conn->o_to_t_network_connection_parameter & 0x01FF;
    conn->produced_connection_size = conn->t_to_o_network_connection_parameter & 0x01FF;
}


EipStatus ForwardClose( CipInstance* instance,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response )
{
    //Suppress compiler warning
    (void) instance;

    // check connection_serial_number && originator_vendor_id && originator_serial_number if connection is established
    ConnectionManagerStatusCode connection_status =
        kConnectionManagerStatusCodeErrorConnectionNotFoundAtTargetApplication;

    CipConn* active = g_active_connection_list;

    // set AddressInfo Items to invalid TypeID to prevent assembleLinearMsg to read them
    g_common_packet_format_data_item.address_info_item[0].type_id   = 0;
    g_common_packet_format_data_item.address_info_item[1].type_id   = 0;

    request->data += 2; // ignore Priority/Time_tick and Time-out_ticks

    EipUint16 connection_serial_number = GetIntFromMessage( &request->data );
    EipUint16 originator_vendor_id = GetIntFromMessage( &request->data );
    EipUint32 originator_serial_number = GetDintFromMessage( &request->data );

    OPENER_TRACE_INFO( "ForwardClose: ConnSerNo %d\n", connection_serial_number );

    while( NULL != active )
    {
        // this check should not be necessary as only established connections should be in the active connection list
        if( (active->state == kConnectionStateEstablished)
            || (active->state == kConnectionStateTimedOut) )
        {
            if( (active->connection_serial_number
                 == connection_serial_number)
                && (active->originator_vendor_id == originator_vendor_id)
                && (active->originator_serial_number
                    == originator_serial_number) )
            {
                // found the corresponding connection object -> close it
                OPENER_ASSERT( NULL != active->connection_close_function );
                active->connection_close_function( active );
                connection_status = kConnectionManagerStatusCodeSuccess;
                break;
            }
        }

        active = active->next_cip_conn;
    }

    return AssembleForwardCloseResponse( connection_serial_number,
            originator_vendor_id,
            originator_serial_number,
            request,
            response,
            connection_status );
}


// TODO: Not implemented
EipStatus GetConnectionOwner( CipInstance* instance,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response )
{
    // suppress compiler warnings
    (void) instance;
    (void) request;
    (void) response;

    return kEipStatusOk;
}


EipStatus ManageConnections()
{
    EipStatus eip_status;

    //Inform application that it can execute
    HandleApplication();
    ManageEncapsulationMessages();

    for( CipConn* active = g_active_connection_list;  active;
            active = active->next_cip_conn )
    {
        if( active->state == kConnectionStateEstablished )
        {
            if( active->consuming_instance                          // We have a consuming connection check
                                                                    // inactivity watchdog timer.

                || (active->transport_type_class_trigger & 0x80) )  // All sever connections have to maintain
                                                                    // an inactivity watchdog timer
            {
                active->inactivity_watchdog_timer -= kOpenerTimerTickInMilliSeconds;

                if( active->inactivity_watchdog_timer <= 0 )
                {
                    // we have a timed out connection: perform watchdog check
                    OPENER_TRACE_INFO( "%s: >>>>>Connection timed out socket[0]:%d socket[1]:%d\n",
                        __func__,
                        active->socket[0], active->socket[1]
                        );

                    OPENER_ASSERT( active->connection_timeout_function );

                    active->connection_timeout_function( active );
                }
            }

            // only if the connection has not timed out check if data is to be send
            if( kConnectionStateEstablished == active->state )
            {
                // client connection
                if( (active->expected_packet_rate != 0) &&
                    (kEipInvalidSocket != active->socket[kUdpCommuncationDirectionProducing]) ) // only produce for the master connection
                {
                    if( kConnectionTriggerTypeCyclicConnection
                        != (active->transport_type_class_trigger & kConnectionTriggerTypeProductionTriggerMask) )
                    {
                        // non cyclic connections have to decrement production inhibit timer
                        if( 0 <= active->production_inhibit_timer )
                        {
                            active->production_inhibit_timer -= kOpenerTimerTickInMilliSeconds;
                        }
                    }

                    active->transmission_trigger_timer -= kOpenerTimerTickInMilliSeconds;

                    if( active->transmission_trigger_timer <= 0 ) // need to send package
                    {
                        OPENER_ASSERT( active->connection_send_data_function );

                        eip_status = active->connection_send_data_function( active );

                        if( eip_status == kEipStatusError )
                        {
                            OPENER_TRACE_ERR( "sending of UDP data in manage Connection failed\n" );
                        }

                        // reload the timer value
                        active->transmission_trigger_timer = active->expected_packet_rate;

                        if( kConnectionTriggerTypeCyclicConnection
                            != (active->transport_type_class_trigger & kConnectionTriggerTypeProductionTriggerMask) )
                        {
                            // non cyclic connections have to reload the production inhibit timer
                            active->production_inhibit_timer = active->production_inhibit_time;
                        }
                    }
                }
            }
        }
    }

    return kEipStatusOk;
}


/* TODO: Update Documentation  INT8 assembleFWDOpenResponse(S_CIP_CipConn *pa_pstConnObj, S_CIP_MR_Response * pa_MRResponse, EIP_UINT8 pa_nGeneralStatus, EIP_UINT16 pa_nExtendedStatus,
 * * deleteMeSomeday, EIP_UINT8 * pa_msg)
 *   create FWDOpen response dependent on status.
 *      pa_pstConnObj pointer to connection Object
 *      pa_MRResponse	pointer to message router response
 *      pa_nGeneralStatus the general status of the response
 *      pa_nExtendedStatus extended status in the case of an error otherwise 0
 *      pa_CPF_data	pointer to CPF Data Item
 *      pa_msg		pointer to memory where reply has to be stored
 *  return status
 *          0 .. no reply need to be sent back
 *          1 .. need to send reply
 *        -1 .. error
 */
EipStatus AssembleForwardOpenResponse( CipConn* co,
        CipMessageRouterResponse* response, EipUint8 general_status,
        EipUint16 extended_status )
{
    // write reply information in CPF struct dependent of pa_status
    CipCommonPacketFormatData* cpfd = &g_common_packet_format_data_item;

    EipByte* message = response->data;

    cpfd->item_count = 2;
    cpfd->data_item.type_id = kCipItemIdUnconnectedDataItem;

    AddNullAddressItem( cpfd );

    response->reply_service  = 0x80 | kForwardOpen;
    response->general_status = general_status;

    if( kCipErrorSuccess == general_status )
    {
        OPENER_TRACE_INFO( "assembleFWDOpenResponse: sending success response\n" );
        response->data_length = 26; // if there is no application specific data
        response->size_of_additional_status = 0;

        if( cpfd->address_info_item[0].type_id != 0 )
        {
            cpfd->item_count = 3;

            if( cpfd->address_info_item[1].type_id != 0 )
            {
                cpfd->item_count = 4; // there are two sockaddrinfo items to add
            }
        }

        AddDintToMessage( co->consumed_connection_id, &message );
        AddDintToMessage( co->produced_connection_id, &message );
    }
    else
    {
        // we have an connection creation error
        OPENER_TRACE_INFO( "assembleFWDOpenResponse: sending error response\n" );
        co->state = kConnectionStateNonExistent;
        response->data_length = 10;

        switch( general_status )
        {
        case kCipErrorNotEnoughData:
        case kCipErrorTooMuchData:
            response->size_of_additional_status = 0;
            break;

        default:
            switch( extended_status )
            {
            case kConnectionManagerStatusCodeErrorInvalidOToTConnectionSize:
                response->size_of_additional_status = 2;
                response->additional_status[0] = extended_status;
                response->additional_status[1] = co->correct_originator_to_target_size;
                break;

            case kConnectionManagerStatusCodeErrorInvalidTToOConnectionSize:
                response->size_of_additional_status = 2;
                response->additional_status[0] = extended_status;
                response->additional_status[1] = co->correct_target_to_originator_size;
                break;

            default:
                response->size_of_additional_status = 1;
                response->additional_status[0] = extended_status;
                break;
            }
            break;
        }
    }

    AddIntToMessage( co->connection_serial_number, &message );
    AddIntToMessage( co->originator_vendor_id, &message );
    AddDintToMessage( co->originator_serial_number, &message );

    if( kCipErrorSuccess == general_status )
    {
        // set the actual packet rate to requested packet rate
        AddDintToMessage( co->o_to_t_requested_packet_interval, &message );
        AddDintToMessage( co->t_to_o_requested_packet_interval, &message );
    }

    *message++ = 0;   // remaining path size - for routing devices relevant
    *message++ = 0;   // reserved

    return kEipStatusOkSend; // send reply
}


/**
 * Adds a Null Address Item to the common data packet format data
 * @param cpfd The CPF data packet where the Null Address Item shall be added
 */
void AddNullAddressItem( CipCommonPacketFormatData* cpfd )
{
    // Precondition: Null Address Item only valid in unconnected messages
    assert( cpfd->data_item.type_id == kCipItemIdUnconnectedDataItem );

    cpfd->address_item.type_id    = kCipItemIdNullAddress;
    cpfd->address_item.length     = 0;
}


/*   INT8 assembleFWDCloseResponse(UINT16 pa_ConnectionSerialNr, UINT16 pa_OriginatorVendorID, UINT32 pa_OriginatorSerialNr, S_CIP_MR_Request *pa_MRRequest, S_CIP_MR_Response *pa_MRResponse, S_CIP_CPF_Data *pa_CPF_data, INT8 pa_status, INT8 *pa_msg)
 *   create FWDClose response dependent on status.
 *      pa_ConnectionSerialNr	requested ConnectionSerialNr
 *      pa_OriginatorVendorID	requested OriginatorVendorID
 *      pa_OriginatorSerialNr	requested OriginalSerialNr
 *      pa_MRRequest		pointer to message router request
 *      pa_MRResponse		pointer to message router response
 *      pa_CPF_data		pointer to CPF Data Item
 *      pa_status		status of FWDClose
 *      pa_msg			pointer to memory where reply has to be stored
 *  return status
 *          0 .. no reply need to ne sent back
 *          1 .. need to send reply
 *             -1 .. error
 */
EipStatus AssembleForwardCloseResponse( EipUint16 connection_serial_number,
        EipUint16 originatior_vendor_id,
        EipUint32 originator_serial_number,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response,
        EipUint16 extended_error_code )
{
    // write reply information in CPF struct dependent of pa_status
    CipCommonPacketFormatData* cpfd = &g_common_packet_format_data_item;
    EipByte* message = response->data;

    cpfd->item_count = 2;
    cpfd->data_item.type_id =
        kCipItemIdUnconnectedDataItem;

    AddNullAddressItem( cpfd );

    AddIntToMessage( connection_serial_number, &message );
    AddIntToMessage( originatior_vendor_id, &message );
    AddDintToMessage( originator_serial_number, &message );

    response->reply_service = (0x80 | request->service);

    response->data_length = 10; // if there is no application specific data

    if( kConnectionManagerStatusCodeSuccess == extended_error_code )
    {
        *message = 0; // no application data
        response->general_status = kCipErrorSuccess;
        response->size_of_additional_status = 0;
    }
    else
    {
        *message = *request->data; // remaining path size
        response->general_status = kCipErrorConnectionFailure;
        response->additional_status[0] = extended_error_code;
        response->size_of_additional_status = 1;
    }

    message++;
    *message = 0; // reserved
    message++;

    return kEipStatusOkSend;
}


CipConn* GetConnectedObject( EipUint32 connection_id )
{
    CipConn* conn = g_active_connection_list;

    while( conn )
    {
        if( conn->state == kConnectionStateEstablished )
        {
            if( conn->consumed_connection_id == connection_id )
                return conn;
        }

        conn = conn->next_cip_conn;
    }

    return NULL;
}


CipConn* GetConnectedOutputAssembly( EipUint32 output_assembly_id )
{
    CipConn* active = g_active_connection_list;

    while( active )
    {
        if( active->state == kConnectionStateEstablished )
        {
            if( active->connection_path.connection_point[0] == output_assembly_id )
                return active;
        }

        active = active->next_cip_conn;
    }

    return NULL;
}


CipConn* CheckForExistingConnection( CipConn* conn )
{
    CipConn* active = g_active_connection_list;

    while( NULL != active )
    {
        if( active->state == kConnectionStateEstablished )
        {
            if( (conn->connection_serial_number == active->connection_serial_number)
                && (conn->originator_vendor_id  == active->originator_vendor_id)
                && (conn->originator_serial_number == active->originator_serial_number) )
            {
                return active;
            }
        }

        active = active->next_cip_conn;
    }

    return NULL;
}


EipStatus CheckElectronicKeyData( EipUint8 key_format, CipKeyData* key_data,
        EipUint16* extended_status )
{
    EipByte compatiblity_mode = key_data->major_revision & 0x80;

    // Remove compatibility bit
    key_data->major_revision &= 0x7F;

    // Default return value
    *extended_status = kConnectionManagerStatusCodeSuccess;

    // Check key format
    if( 4 != key_format )
    {
        *extended_status = kConnectionManagerStatusCodeErrorInvalidSegmentTypeInPath;
        return kEipStatusError;
    }

    // Check VendorID and ProductCode, must match, or 0
    if( ( (key_data->vendor_id != vendor_id_) && (key_data->vendor_id != 0) )
        || ( (key_data->product_code != product_code_)
             && (key_data->product_code != 0) ) )
    {
        *extended_status = kConnectionManagerStatusCodeErrorVendorIdOrProductcodeError;
        return kEipStatusError;
    }
    else
    {
        // VendorID and ProductCode are correct

        // Check DeviceType, must match or 0
        if( (key_data->device_type != device_type_) && (key_data->device_type != 0) )
        {
            *extended_status = kConnectionManagerStatusCodeErrorDeviceTypeError;
            return kEipStatusError;
        }
        else
        {
            // VendorID, ProductCode and DeviceType are correct

            if( !compatiblity_mode )
            {
                // Major = 0 is valid
                if( 0 == key_data->major_revision )
                {
                    return kEipStatusOk;
                }

                // Check Major / Minor Revision, Major must match, Minor match or 0
                if( (key_data->major_revision != revision_.major_revision)
                    || ( (key_data->minor_revision != revision_.minor_revision)
                         && (key_data->minor_revision != 0) ) )
                {
                    *extended_status = kConnectionManagerStatusCodeErrorRevisionMismatch;
                    return kEipStatusError;
                }
            }
            else
            {
                // Compatibility mode is set

                // Major must match, Minor != 0 and <= MinorRevision
                if( (key_data->major_revision == revision_.major_revision)
                    && (key_data->minor_revision > 0)
                    && (key_data->minor_revision <= revision_.minor_revision) )
                {
                    return kEipStatusOk;
                }
                else
                {
                    *extended_status = kConnectionManagerStatusCodeErrorRevisionMismatch;
                    return kEipStatusError;
                }
            } // end if CompatiblityMode handling
        }
    }

    return (*extended_status == kConnectionManagerStatusCodeSuccess) ?
           kEipStatusOk : kEipStatusError;
}


EipUint8 ParseConnectionPath( CipConn* conn,
        CipMessageRouterRequest* request,
        EipUint16* extended_error )
{
    EipUint8* message = request->data;
    int remaining_path_size = conn->connection_path_size = *message++; // length in words

    CipClass* clazz = NULL;

    int originator_to_target_connection_type;
    int target_to_originator_connection_type;

    // with 256 we mark that we haven't got a PIT segment
    conn->production_inhibit_time = 256;

    if( (g_kForwardOpenHeaderLength + remaining_path_size * 2) < request->data_length )
    {
        // the received packet is larger than the data in the path
        *extended_error = 0;
        return kCipErrorTooMuchData;
    }

    if( (g_kForwardOpenHeaderLength + remaining_path_size * 2) > request->data_length )
    {
        //there is not enough data in received packet
        *extended_error = 0;
        return kCipErrorNotEnoughData;
    }

    if( remaining_path_size > 0 )
    {
        // first electronic key
        if( *message == 0x34 )
        {
            if( remaining_path_size < 5 )
            {
                //there is not enough data for holding the electronic key segment
                *extended_error = 0;
                return kCipErrorNotEnoughData;
            }

            // logical electronic key found
            conn->electronic_key.segment_type = 0x34;
            message++;
            conn->electronic_key.key_format = *message++;
            conn->electronic_key.key_data.vendor_id = GetIntFromMessage( &message );
            conn->electronic_key.key_data.device_type =  GetIntFromMessage( &message );
            conn->electronic_key.key_data.product_code = GetIntFromMessage( &message );
            conn->electronic_key.key_data.major_revision   = *message++;
            conn->electronic_key.key_data.minor_revision   = *message++;
            remaining_path_size -= 5; //length of the electronic key

            OPENER_TRACE_INFO(
                    "key: ven ID %d, dev type %d, prod code %d, major %d, minor %d\n",
                    conn->electronic_key.key_data.vendor_id,
                    conn->electronic_key.key_data.device_type,
                    conn->electronic_key.key_data.product_code,
                    conn->electronic_key.key_data.major_revision,
                    conn->electronic_key.key_data.minor_revision );

            if( kEipStatusOk != CheckElectronicKeyData(
                        conn->electronic_key.key_format,
                        &(conn->electronic_key.key_data), extended_error ) )
            {
                return kCipErrorConnectionFailure;
            }
        }
        else
        {
            OPENER_TRACE_INFO( "no key\n" );
        }

        if( kConnectionTriggerTypeCyclicConnection !=
            (conn->transport_type_class_trigger & kConnectionTriggerTypeProductionTriggerMask) )
        {
            // non cyclic connections may have a production inhibit
            if( kProductionTimeInhibitTimeNetworkSegment == *message )
            {
                conn->production_inhibit_time = message[1];
                message += 2;
                remaining_path_size -= 1;
            }
        }

        if( EQLOGICALPATH( *message, 0x20 ) ) // classID
        {
            conn->connection_path.class_id = GetPaddedLogicalPath( &message );

            clazz = GetCipClass( conn->connection_path.class_id );

            if( 0 == clazz )
            {
                OPENER_TRACE_ERR( "classid %d not found\n",
                        (int) conn->connection_path.class_id );

                if( conn->connection_path.class_id >= 0xC8 ) //reserved range of class ids
                {
                    *extended_error =
                        kConnectionManagerStatusCodeErrorInvalidSegmentTypeInPath;
                }
                else
                {
                    *extended_error =
                        kConnectionManagerStatusCodeInconsistentApplicationPathCombo;
                }

                return kCipErrorConnectionFailure;
            }

            OPENER_TRACE_INFO( "classid %d (%s)\n",
                    (int) conn->connection_path.class_id,
                    clazz->class_name.c_str() );
        }
        else
        {
            *extended_error =
                kConnectionManagerStatusCodeErrorInvalidSegmentTypeInPath;
            return kCipErrorConnectionFailure;
        }

        remaining_path_size -= 1;               // 1 16Bit word for the class part of the path

        if( EQLOGICALPATH( *message, 0x24 ) )   // store the configuration ID for later checking in the application connection types
        {
            conn->connection_path.connection_point[2] = GetPaddedLogicalPath( &message );

            OPENER_TRACE_INFO( "Configuration instance id %" PRId32 "\n",
                    conn->connection_path.connection_point[2] );

            if( NULL == GetCipInstance( clazz, conn->connection_path.connection_point[2] ) )
            {
                //according to the test tool we should respond with this extended error code
                *extended_error =
                    kConnectionManagerStatusCodeErrorInvalidSegmentTypeInPath;
                return kCipErrorConnectionFailure;
            }

            // 1 or 2 16Bit words for the configuration instance part of the path
            remaining_path_size -=
                (conn->connection_path.connection_point[2] > 0xFF) ?
                2 : 1;
        }
        else
        {
            OPENER_TRACE_INFO( "no config data\n" );
        }

        if( 0x03 == (conn->transport_type_class_trigger & 0x03) )
        {
            // we have Class 3 connection
            if( remaining_path_size > 0 )
            {
                OPENER_TRACE_WARN(
                        "Too much data in connection path for class 3 connection\n" );
                *extended_error =
                    kConnectionManagerStatusCodeErrorInvalidSegmentTypeInPath;
                return kCipErrorConnectionFailure;
            }

            // connection end point has to be the message router instance 1
            if( (conn->connection_path.class_id
                 != kCipMessageRouterClassCode)
                || (conn->connection_path.connection_point[2] != 1) )
            {
                *extended_error =
                    kConnectionManagerStatusCodeInconsistentApplicationPathCombo;
                return kCipErrorConnectionFailure;
            }

            conn->connection_path.connection_point[0] = conn->connection_path.connection_point[2];
        }
        else // we have an IO connection
        {
            originator_to_target_connection_type =
                (conn->o_to_t_network_connection_parameter & 0x6000) >> 13;

            target_to_originator_connection_type =
                (conn->t_to_o_network_connection_parameter & 0x6000) >> 13;

            conn->connection_path.connection_point[1] = 0; // set not available path to Invalid

            int number_of_encoded_paths = 0;

            if( originator_to_target_connection_type == 0 )
            {
                if( target_to_originator_connection_type == 0 ) // configuration only connection
                {
                    number_of_encoded_paths = 0;
                    OPENER_TRACE_WARN( "assembly: type invalid\n" );
                }
                else // 1 path -> path is for production
                {
                    OPENER_TRACE_INFO( "assembly: type produce\n" );
                    number_of_encoded_paths = 1;
                }
            }
            else
            {
                if( target_to_originator_connection_type == 0 ) // 1 path -> path is for consumption
                {
                    OPENER_TRACE_INFO( "assembly: type consume\n" );
                    number_of_encoded_paths = 1;
                }
                else // 2 paths -> 1st for production 2nd for consumption
                {
                    OPENER_TRACE_INFO( "assembly: type bidirectional\n" );
                    number_of_encoded_paths = 2;
                }
            }

            for( int i = 0; i < number_of_encoded_paths; i++ )                              // process up to 2 encoded paths
            {
                if( EQLOGICALPATH( *message, 0x24 ) || EQLOGICALPATH( *message, 0x2C ) )    // Connection Point interpreted as InstanceNr -> only in Assembly Objects
                {
                    // InstanceNR
                    conn->connection_path.connection_point[i] = GetPaddedLogicalPath( &message );

                    OPENER_TRACE_INFO(
                            "connection point %u\n",
                            (unsigned) conn->connection_path.connection_point[i] );

                    if( 0 == GetCipInstance( clazz, conn->connection_path.connection_point[i] ) )
                    {
                        *extended_error =
                            kConnectionManagerStatusCodeInconsistentApplicationPathCombo;
                        return kCipErrorConnectionFailure;
                    }

                    // 1 or 2 16Bit word for the connection point part of the path
                    remaining_path_size -=
                        (conn->connection_path.connection_point[i] > 0xFF) ? 2 : 1;
                }
                else
                {
                    *extended_error =
                        kConnectionManagerStatusCodeErrorInvalidSegmentTypeInPath;
                    return kCipErrorConnectionFailure;
                }
            }

            g_config_data_length    = 0;
            g_config_data_buffer    = NULL;

            while( remaining_path_size > 0 ) // have something left in the path should be configuration data
            {
                switch( *message )
                {
                case kDataSegmentTypeSimpleDataMessage:
                    // we have a simple data segment
                    g_config_data_length    = message[1] * 2; //data segments store length 16-bit word wise
                    g_config_data_buffer    = &(message[2]);
                    remaining_path_size     -= (g_config_data_length + 2);
                    message += (g_config_data_length + 2);
                    break;

                //TODO do we have to handle ANSI extended symbol data segments too?
                case kProductionTimeInhibitTimeNetworkSegment:

                    if( kConnectionTriggerTypeCyclicConnection
                        != (conn->transport_type_class_trigger
                            & kConnectionTriggerTypeProductionTriggerMask) )
                    {
                        // only non cyclic connections may have a production inhibit
                        conn->production_inhibit_time = message[1];
                        message += 2;
                        remaining_path_size -= 2;
                    }
                    else
                    {
                        *extended_error = conn->connection_path_size
                                          - remaining_path_size;    //offset in 16Bit words where within the connection path the error happend
                        return kCipErrorPathSegmentError;           //status code for invalid segment type
                    }
                    break;

                default:
                    OPENER_TRACE_WARN(
                            "No data segment identifier found for the configuration data\n" );

                    *extended_error = conn->connection_path_size
                                      - remaining_path_size;    //offset in 16Bit words where within the connection path the error happend

                    return 0x04;                                //status code for invalid segment type
                    break;
                }
            }
        }
    }

    //save back the current position in the stream allowing followers to parse anything thats still there
    request->data = message;
    return kEipStatusOk;
}


void CloseConnection( CipConn* pa_pstConnObj )
{
    pa_pstConnObj->state = kConnectionStateNonExistent;

    if( 0x03 != (pa_pstConnObj->transport_type_class_trigger & 0x03) )
    {
        // only close the UDP connection for not class 3 connections
        IApp_CloseSocket_udp(
                pa_pstConnObj->socket[kUdpCommuncationDirectionConsuming] );
        pa_pstConnObj->socket[kUdpCommuncationDirectionConsuming] =
            kEipInvalidSocket;
        IApp_CloseSocket_udp(
                pa_pstConnObj->socket[kUdpCommuncationDirectionProducing] );
        pa_pstConnObj->socket[kUdpCommuncationDirectionProducing] =
            kEipInvalidSocket;
    }

    RemoveFromActiveConnections( pa_pstConnObj );
}


void CopyConnectionData( CipConn* pa_pstDst, CipConn* pa_pstSrc )
{
    memcpy( pa_pstDst, pa_pstSrc, sizeof(CipConn) );
}


void AddNewActiveConnection( CipConn* pa_pstConn )
{
    pa_pstConn->first_cip_conn = NULL;
    pa_pstConn->next_cip_conn  = g_active_connection_list;

    if( NULL != g_active_connection_list )
    {
        g_active_connection_list->first_cip_conn = pa_pstConn;
    }

    g_active_connection_list = pa_pstConn;
    g_active_connection_list->state = kConnectionStateEstablished;
}


void RemoveFromActiveConnections( CipConn* pa_pstConn )
{
    if( NULL != pa_pstConn->first_cip_conn )
    {
        pa_pstConn->first_cip_conn->next_cip_conn =
            pa_pstConn->next_cip_conn;
    }
    else
    {
        g_active_connection_list = pa_pstConn->next_cip_conn;
    }

    if( NULL != pa_pstConn->next_cip_conn )
    {
        pa_pstConn->next_cip_conn->first_cip_conn =
            pa_pstConn->first_cip_conn;
    }

    pa_pstConn->first_cip_conn = NULL;
    pa_pstConn->next_cip_conn  = NULL;
    pa_pstConn->state = kConnectionStateNonExistent;
}


EipBool8 IsConnectedOutputAssembly( EipUint32 pa_nInstanceNr )
{
    EipBool8 bRetVal = false;

    CipConn* pstRunner = g_active_connection_list;

    while( NULL != pstRunner )
    {
        if( pa_nInstanceNr == pstRunner->connection_path.connection_point[0] )
        {
            bRetVal = true;
            break;
        }

        pstRunner = pstRunner->next_cip_conn;
    }

    return bRetVal;
}


EipStatus AddConnectableObject( EipUint32 aClassId, OpenConnectionFunction func )
{
    // parsing is now finished all data is available and check now establish the connection
    for( int i = 0; i < NUM_CONNECTABLE_OBJECTS; ++i )
    {
        if( !g_astConnMgmList[i].class_id || aClassId == g_astConnMgmList[i].class_id )
        {
            OPENER_TRACE_INFO(
                "%s: adding classId %d with function ptr %p at index %d\n",
                __func__, aClassId, func, i
                );

            g_astConnMgmList[i].class_id = aClassId;
            g_astConnMgmList[i].open_connection_function = func;

            table_init = true;

            return kEipStatusOk;
        }
    }

    OPENER_TRACE_INFO( "%s: unable to aClassId:%d add\n", __func__, aClassId );

    return kEipStatusError;
}


ConnectionManagementHandling* GetConnMgmEntry( EipUint32 class_id )
{
    for( int i = 0;  i < NUM_CONNECTABLE_OBJECTS;  ++i )
    {
        OPENER_TRACE_INFO( "%s: [%d]:class_id: %d\n",
            __func__, i, g_astConnMgmList[i].class_id );

        if( class_id == g_astConnMgmList[i].class_id )
        {
            OPENER_TRACE_ERR( "%s: found class %d at entry %d\n", __func__, class_id, i );
            return &g_astConnMgmList[i];
        }
    }

    OPENER_TRACE_ERR( "%s: could not find class %d\n", __func__, class_id );

    return NULL;
}


EipStatus TriggerConnections( unsigned pa_unOutputAssembly,
        unsigned pa_unInputAssembly )
{
    EipStatus nRetVal = kEipStatusError;

    CipConn* pstRunner = g_active_connection_list;

    while( NULL != pstRunner )
    {
        if( (pa_unOutputAssembly == pstRunner->connection_path.connection_point[0])
            && (pa_unInputAssembly == pstRunner->connection_path.connection_point[1]) )
        {
            if( kConnectionTriggerTypeApplicationTriggeredConnection
                == (pstRunner->transport_type_class_trigger
                    & kConnectionTriggerTypeProductionTriggerMask) )
            {
                // produce at the next allowed occurrence
                pstRunner->transmission_trigger_timer =
                    pstRunner->production_inhibit_timer;
                nRetVal = kEipStatusOk;
            }

            break;
        }
    }

    return nRetVal;
}


void InitializeConnectionManagerData()
{
    OPENER_TRACE_INFO( "%s: \n", __func__ );

    memset( g_astConnMgmList, 0, sizeof g_astConnMgmList );

    InitializeClass3ConnectionData();
    InitializeIoConnectionData();
}
