/*******************************************************************************
 * Copyright (c) 2009, Rockwell Automation, Inc.
 * All rights reserved.
 *
 ******************************************************************************/
#include "opener_api.h"
#include "cipcommon.h"
#include "cipmessagerouter.h"
#include "endianconv.h"
#include "ciperror.h"
#include "trace.h"

CipMessageRouterRequest     g_request;
CipMessageRouterResponse    g_response;


/** @brief A class registry list node
 *
 * A linked list of this  object is the registry of classes known to the message router.
 */
struct CipMessageRouterObject
{
    CipMessageRouterObject* next;
    CipClass*               cip_class;
};


/// @brief Pointer to first registered object in MessageRouter
CipMessageRouterObject* g_first_object;


/** @brief Register an Class to the message router
 *  @param cip_class Pointer to a class object to be registered.
 *  @return status      0 .. success
 *                     -1 .. error no memory available to register more objects
 */
EipStatus RegisterCipClass( CipClass* cip_class );

/** @brief Create Message Router Request structure out of the received data.
 *
 * Parses the UCMM header consisting of: service, IOI size, IOI, data into a request structure
 * @param data pointer to the message data received
 * @param data_length number of bytes in the message
 * @param request pointer to structure of MRRequest data item.
 * @return status  0 .. success
 *                 -1 .. error
 */
CipError CreateMessageRouterRequestStructure( EipUint8* data, EipInt16 data_length,
        CipMessageRouterRequest* request );

EipStatus CipMessageRouterInit()
{
    CipClass* message_router;

    message_router = CreateCipClass( kCipMessageRouterClassCode,    // class ID
            0,                                                      // # of class attributes
            0xffffffff,                                             // class getAttributeAll mask
            0,                                                      // # of class services
            0,                                                      // # of instance attributes
            0xffffffff,                                             // instance getAttributeAll mask
            0,                                                      // # of instance services
            1,                                                      // # of instances
            "message router",                                       // class name
            1 );                                                    // revision

    if( message_router == 0 )
        return kEipStatusError;

    // reserved for future use -> set to zero
    g_response.reserved = 0;
    g_response.data = g_message_data_reply_buffer; // set reply buffer, using a fixed buffer (about 100 bytes)

    return kEipStatusOk;
}


/** @brief Get the registered MessageRouter object corresponding to ClassID.
 *  given a class ID, return a pointer to the registration node for that object
 *
 *  @param class_id Class code to be searched for.
 *  @return Pointer to registered message router object
 *      0 .. Class not registered
 */
CipMessageRouterObject* GetRegisteredObject( EipUint32 class_id )
{
    CipMessageRouterObject* object = g_first_object;    // get pointer to head of class registration list

    while( NULL != object )                             // for each entry in list
    {
        OPENER_ASSERT( object->cip_class != NULL );

        if( object->cip_class->class_id == class_id )
            return object; // return registration node if it matches class ID

        object = object->next;
    }

    return 0;
}


CipClass* GetCipClass( EipUint32 class_id )
{
    CipMessageRouterObject* p = GetRegisteredObject( class_id );

    if( p )
        return p->cip_class;
    else
        return NULL;
}


CipInstance* GetCipInstance( CipClass* cip_class, EipUint32 instance_id )
{
    // if the instance number is zero, return the class object itself
    if( instance_id == 0 )
        return (CipInstance*) cip_class;

    CipClass::CipInstances& instances = cip_class->instances;
    for( unsigned i = 0; i < instances.size();  ++i )
    {
        if( instances[i]->instance_id == instance_id )
            return instances[i];
    }

    return NULL;
}


EipStatus RegisterCipClass( CipClass* cip_class )
{
    CipMessageRouterObject** message_router_object = &g_first_object;

    while( *message_router_object )
        message_router_object = &(*message_router_object)->next; // follow the list until p points to an empty link (list end)

    *message_router_object = (CipMessageRouterObject*) CipCalloc(
            1, sizeof(CipMessageRouterObject) ); // create a new node at the end of the list

    if( *message_router_object == 0 )
        return kEipStatusError;                         // check for memory error

    (*message_router_object)->cip_class = cip_class;    // fill in the new node
    (*message_router_object)->next = NULL;

    return kEipStatusOk;
}


EipStatus NotifyMR( EipUint8* data, int data_length )
{
    EipStatus   eip_status = kEipStatusOkSend;
    EipByte     nStatus;

    g_response.data = g_message_data_reply_buffer; // set reply buffer, using a fixed buffer (about 100 bytes)

    OPENER_TRACE_INFO( "notifyMR: routing unconnected message\n" );

    if( kCipErrorSuccess
        != ( nStatus = CreateMessageRouterRequestStructure(
                     data, data_length, &g_request ) ) ) // error from create MR structure
    {
        OPENER_TRACE_ERR( "notifyMR: error from createMRRequeststructure\n" );
        g_response.general_status = nStatus;
        g_response.size_of_additional_status = 0;
        g_response.reserved = 0;
        g_response.data_length = 0;
        g_response.reply_service = (0x80
                                                   | g_request.service);
    }
    else
    {
        // forward request to appropriate Object if it is registered
        CipMessageRouterObject* registered_object;

        registered_object = GetRegisteredObject(
                g_request.request_path.class_id );

        if( registered_object == 0 )
        {
            OPENER_TRACE_ERR(
                    "notifyMR: sending CIP_ERROR_OBJECT_DOES_NOT_EXIST reply, class id 0x%x is not registered\n",
                    (unsigned) g_request.request_path.class_id );
            g_response.general_status =
                kCipErrorPathDestinationUnknown; //according to the test tool this should be the correct error flag instead of CIP_ERROR_OBJECT_DOES_NOT_EXIST;
            g_response.size_of_additional_status = 0;
            g_response.reserved = 0;
            g_response.data_length = 0;
            g_response.reply_service = (0x80
                                                       | g_request.service);
        }
        else
        {
            /* call notify function from Object with ClassID (gMRRequest.RequestPath.ClassID)
             *  object will or will not make an reply into gMRResponse*/
            g_response.reserved = 0;
            OPENER_ASSERT( NULL != registered_object->cip_class );

            OPENER_TRACE_INFO( "notifyMR: calling notify function of class '%s'\n",
                    registered_object->cip_class->class_name.c_str() );

            eip_status = NotifyClass( registered_object->cip_class,
                    &g_request,
                    &g_response );

#ifdef OPENER_TRACE_ENABLED

            if( eip_status == kEipStatusError )
            {
                OPENER_TRACE_ERR(
                        "notifyMR: notify function of class '%s' returned an error\n",
                        registered_object->cip_class->class_name.c_str() );
            }
            else if( eip_status == kEipStatusOk )
            {
                OPENER_TRACE_INFO(
                        "notifyMR: notify function of class '%s' returned no reply\n",
                        registered_object->cip_class->class_name.c_str() );
            }
            else
            {
                OPENER_TRACE_INFO(
                        "notifyMR: notify function of class '%s' returned a reply\n",
                        registered_object->cip_class->class_name.c_str() );
            }

#endif
        }
    }

    return eip_status;
}


CipError CreateMessageRouterRequestStructure( EipUint8* data, EipInt16 data_length,
        CipMessageRouterRequest* request )
{
    int number_of_decoded_bytes;

    request->service = *data;
    data++; //TODO: Fix for 16 bit path lengths (+1
    data_length--;

    number_of_decoded_bytes = DecodePaddedEPath(
            &(request->request_path), &data );

    if( number_of_decoded_bytes < 0 )
    {
        return kCipErrorPathSegmentError;
    }

    request->data = data;
    request->data_length = data_length - number_of_decoded_bytes;

    if( request->data_length < 0 )
        return kCipErrorPathSizeInvalid;
    else
        return kCipErrorSuccess;
}


void DeleteAllClasses()
{
    CipMessageRouterObject* mro = g_first_object; // get pointer to head of class registration list

    while( mro )
    {
        CipMessageRouterObject* del_mro = mro;

        mro = mro->next;

        delete del_mro->cip_class;

        delete del_mro;
    }

    g_first_object = NULL;
}
