/*******************************************************************************
 * Copyright (c) 2009, Rockwell Automation, Inc.
 * All rights reserved.
 *
 ******************************************************************************/
#include <string.h>

#include "cipcommon.h"

#include "trace.h"
#include "opener_api.h"
#include "cipidentity.h"
#include "ciptcpipinterface.h"
#include "cipethernetlink.h"
#include "cipconnectionmanager.h"
#include "endianconv.h"
#include "encap.h"
#include "ciperror.h"
#include "cipassembly.h"
#include "cipmessagerouter.h"
#include "cpf.h"
#include "appcontype.h"

/* global public variables */
EipUint8 g_message_data_reply_buffer[OPENER_MESSAGE_DATA_REPLY_BUFFER];

const EipUint16 kCipUintZero = 0;

/* private functions*/
int EncodeEPath( CipEpath* epath, EipUint8** message );

void CipStackInit( EipUint16 unique_connection_id )
{
    EipStatus eip_status;

    EncapsulationInit();

    /* The message router is the first CIP object be initialized!!! */
    eip_status = CipMessageRouterInit();
    OPENER_ASSERT( kEipStatusOk == eip_status );

    eip_status = CipIdentityInit();
    OPENER_ASSERT( kEipStatusOk == eip_status );

    eip_status = CipTcpIpInterfaceInit();
    OPENER_ASSERT( kEipStatusOk == eip_status );

    eip_status = CipEthernetLinkInit();
    OPENER_ASSERT( kEipStatusOk == eip_status );

    eip_status = ConnectionManagerInit( unique_connection_id );
    OPENER_ASSERT( kEipStatusOk == eip_status );

    eip_status = CipAssemblyInitialize();
    OPENER_ASSERT( kEipStatusOk == eip_status );

#if 0    // do this in caller after return from this function.
    /* the application has to be initialized at last */
    eip_status = ApplicationInitialization();
    OPENER_ASSERT( kEipStatusOk == eip_status );
#endif

    (void) eip_status;
}


void ShutdownCipStack( void )
{
    /* First close all connections */
    CloseAllConnections();

    /* Than free the sockets of currently active encapsulation sessions */
    EncapsulationShutDown();

    /*clean the data needed for the assembly object's attribute 3*/
    ShutdownAssemblies();

    ShutdownTcpIpInterface();

    /*no clear all the instances and classes */
    DeleteAllClasses();
}


EipStatus NotifyClass( CipClass* cip_class,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response )
{
    int i;
    CipInstance* instance;
    CipServiceStruct* service;
    unsigned instance_number; /* my instance number */

    /* find the instance: if instNr==0, the class is addressed, else find the instance */
    instance_number = request->request_path.instance_number; /* get the instance number */
    instance = GetCipInstance( cip_class, instance_number );                /* look up the instance (note that if inst==0 this will be the class itself) */

    if( instance )                                                          /* if instance is found */
    {
        OPENER_TRACE_INFO( "notify: found instance %d%s\n", instance_number,
                instance_number == 0 ? " (class object)" : "" );

        service = instance->cip_class->services;                                    /* get pointer to array of services */

        if( service )                                                               /* if services are defined */
        {
            for( i = 0; i < instance->cip_class->number_of_services; i++ )          /* seach the services list */
            {
                if( request->service == service->service_number )    /* if match is found */
                {
                    /* call the service, and return what it returns */
                    OPENER_TRACE_INFO( "notify: calling %s service\n", service->name );
                    OPENER_ASSERT( NULL != service->service_function );
                    return service->service_function( instance, request,
                            response );
                }
                else
                {
                    service++;
                }
            }
        }

        OPENER_TRACE_WARN( "notify: service 0x%x not supported\n",
                request->service );
        response->general_status = kCipErrorServiceNotSupported; /* if no services or service not found, return an error reply*/
    }
    else
    {
        OPENER_TRACE_WARN( "notify: instance number %d unknown\n", instance_number );

        /* if instance not found, return an error reply*/
        response->general_status = kCipErrorPathDestinationUnknown; /*according to the test tool this should be the correct error flag instead of CIP_ERROR_OBJECT_DOES_NOT_EXIST;*/
    }

    /* handle error replies*/
    response->size_of_additional_status = 0; /* fill in the rest of the reply with not much of anything*/
    response->data_length = 0;
    response->reply_service = (0x80 | request->service); /* except the reply code is an echo of the command + the reply flag */

    return kEipStatusOkSend;
}


CipInstance* AddCipInstances( CipClass* cip_class, int number_of_instances )
{
    CipInstance* first_instance, * current_instance, ** next_instance;
    int i;
    int instance_number = 1; /* the first instance is number 1 */

    OPENER_TRACE_INFO( "adding %d instances to class %s\n", number_of_instances,
            cip_class->class_name );

    next_instance = &cip_class->instances;          /* get address of pointer to head of chain */

    while( *next_instance )                         /* as long as what pp points to is not zero */
    {
        next_instance = &(*next_instance)->next;    /*    follow the chain until pp points to pointer that contains a zero */
        instance_number++;                          /*    keep track of what the first new instance number will be */
    }

    first_instance = current_instance = (CipInstance*) CipCalloc(
            number_of_instances, sizeof(CipInstance) ); /* allocate a block of memory for all created instances*/

    OPENER_ASSERT( NULL != current_instance );
    /* fail if run out of memory */

    cip_class->number_of_instances += number_of_instances;      /* add the number of instances just created to the total recorded by the class */

    for( i = 0; i < number_of_instances; i++ )                  /* initialize all the new instances */
    {
        *next_instance = current_instance;                      /* link the previous pointer to this new node */

        current_instance->instance_number = instance_number;    /* assign the next sequential instance number */
        current_instance->cip_class = cip_class;                /* point each instance to its class */

        if( cip_class->number_of_attributes )                   /* if the class calls for instance attributes */
        {
            /* then allocate storage for the attribute array */
            current_instance->attributes = (CipAttributeStruct*) CipCalloc(
                    cip_class->number_of_attributes, sizeof(CipAttributeStruct) );
        }

        next_instance = &current_instance->next;    /* update pp to point to the next link of the current node */
        instance_number++;                          /* update to the number of the next node*/
        current_instance++;                         /* point to the next node in the calloc'ed array*/
    }

    return first_instance;
}


CipInstance* AddCIPInstance( CipClass* class, EipUint32 instance_id )
{
    CipInstance* instance = GetCipInstance( class, instance_id );

    if( 0 == instance ) /*we have no instance with given id*/
    {
        instance = AddCipInstances( class, 1 );
        instance->instance_number = instance_id;
    }

    return instance;
}


CipClass* CreateCipClass( EipUint32 class_id, int number_of_class_attributes,
        EipUint32 get_all_class_attributes_mask,
        int number_of_class_services,
        int number_of_instance_attributes,
        EipUint32 get_all_instance_attributes_mask,
        int number_of_instance_services,
        int number_of_instances, char* name,
        EipUint16 revision )
{
    OPENER_TRACE_INFO( "creating class '%s' with id: 0x%" PRIX32 "\n", name,
            class_id );

    OPENER_ASSERT( !GetCipClass( class_id ) );   /* should never try to redefine a class*/

    /* a metaClass is a class that holds the class attributes and services
     *  CIP can talk to an instance, therefore an instance has a pointer to its class
     *  CIP can talk to a class, therefore a class struct is a subclass of the instance struct,
     *  and contains a pointer to a metaclass
     *  CIP never explicitly addresses a metaclass*/

    CipClass* clazz = (CipClass*) CipCalloc( 1, sizeof(CipClass) );       /* create the class object*/
    CipClass* meta_class = (CipClass*) CipCalloc( 1, sizeof(CipClass) );  /* create the metaclass object*/

    /* initialize the class-specific fields of the Class struct*/
    clazz->class_id = class_id;                                                             /* the class remembers the class ID */
    clazz->revision = revision;                                                             /* the class remembers the class ID */
    clazz->number_of_instances = 0;                                                         /* the number of instances initially zero (more created below) */
    clazz->instances = 0;
    clazz->number_of_attributes = number_of_instance_attributes;                            /* the class remembers the number of instances of that class */
    clazz->get_attribute_all_mask = get_all_instance_attributes_mask;                       /* indicate which attributes are included in instance getAttributeAll */
    clazz->number_of_services = number_of_instance_services
                                + ( (0 == get_all_instance_attributes_mask) ? 1 : 2 );      /* the class manages the behavior of the instances */
    clazz->services = 0;
    clazz->class_name = name;                                                               /* initialize the class-specific fields of the metaClass struct */
    meta_class->class_id = 0xffffffff;                                                      /* set metaclass ID (this should never be referenced) */
    meta_class->number_of_instances = 1;                                                    /* the class object is the only instance of the metaclass */
    meta_class->instances = (CipInstance*) clazz;
    meta_class->number_of_attributes = number_of_class_attributes + 7;                      /* the metaclass remembers how many class attributes exist*/
    meta_class->get_attribute_all_mask = get_all_class_attributes_mask;                     /* indicate which attributes are included in class getAttributeAll*/
    meta_class->number_of_services = number_of_class_services
                                     + ( (0 == get_all_class_attributes_mask) ? 1 : 2 );    /* the metaclass manages the behavior of the class itself */
    clazz->services = 0;
    meta_class->class_name = (char*) CipCalloc( 1, strlen( name ) + 6 );                    /* fabricate the name "meta<classname>"*/
    strcpy( meta_class->class_name, "meta-" );
    strcat( meta_class->class_name, name );

    /* initialize the instance-specific fields of the Class struct*/
    clazz->m_stSuper.instance_number = 0;               /* the class object is instance zero of the class it describes (weird, but that's the spec)*/
    clazz->m_stSuper.attributes = 0;                    /* this will later point to the class attibutes*/
    clazz->m_stSuper.cip_class  = meta_class;           /* the class's class is the metaclass (like SmallTalk)*/
    clazz->m_stSuper.next = 0;                          /* the next link will always be zero, sinc there is only one instance of any particular class object */

    meta_class->m_stSuper.instance_number = 0xffffffff; /*the metaclass object does not really have a valid instance number*/
    meta_class->m_stSuper.attributes = 0;               /* the metaclass has no attributes*/
    meta_class->m_stSuper.cip_class = 0;                /* the metaclass has no class*/
    meta_class->m_stSuper.next = 0;                     /* the next link will always be zero, since there is only one instance of any particular metaclass object*/

    /* further initialization of the class object*/

    clazz->m_stSuper.attributes = (CipAttributeStruct*) CipCalloc(
            meta_class->number_of_attributes, sizeof(CipAttributeStruct) );
    /* TODO -- check that we didn't run out of memory?*/

    meta_class->services = (CipServiceStruct*) CipCalloc(
            meta_class->number_of_services, sizeof(CipServiceStruct) );

    clazz->services = (CipServiceStruct*) CipCalloc( clazz->number_of_services,
            sizeof(CipServiceStruct) );

    if( number_of_instances > 0 )
    {
        AddCipInstances( clazz, number_of_instances ); /*TODO handle return value and clean up if necessary*/
    }

    if( ( RegisterCipClass( clazz ) ) == kEipStatusError )  /* no memory to register class in Message Router */
    {
        return 0;                                           /*TODO handle return value and clean up if necessary*/
    }

    /* create the standard class attributes*/
    InsertAttribute( (CipInstance*) clazz, 1, kCipUint, (void*) &clazz->revision,
            kGetableSingleAndAll );                                         /* revision */

    InsertAttribute( (CipInstance*) clazz, 2, kCipUint,
            (void*) &clazz->number_of_instances, kGetableSingleAndAll );    /*  largest instance number */

    InsertAttribute( (CipInstance*) clazz, 3, kCipUint,
            (void*) &clazz->number_of_instances, kGetableSingleAndAll );    /* number of instances currently existing*/

    InsertAttribute( (CipInstance*) clazz, 4, kCipUint, (void*) &kCipUintZero,
            kGetableAll );                                                  /* optional attribute list - default = 0 */

    InsertAttribute( (CipInstance*) clazz, 5, kCipUint, (void*) &kCipUintZero,
            kGetableAll );                                                  /* optional service list - default = 0 */

    InsertAttribute( (CipInstance*) clazz, 6, kCipUint,
            (void*) &meta_class->highest_attribute_number,
            kGetableSingleAndAll );      /* max class attribute number*/

    InsertAttribute( (CipInstance*) clazz, 7, kCipUint,
            (void*) &clazz->highest_attribute_number,
            kGetableSingleAndAll );      /* max instance attribute number*/

    /* create the standard class services*/
    if( 0 != get_all_class_attributes_mask ) /*only if the mask has values add the get_attribute_all service */
    {
        InsertService( meta_class, kGetAttributeAll, &GetAttributeAll,
                "GetAttributeAll" );  /* bind instance services to the metaclass*/
    }

    InsertService( meta_class, kGetAttributeSingle, &GetAttributeSingle,
            "GetAttributeSingle" );

    /* create the standard instance services*/
    if( 0 != get_all_instance_attributes_mask )                                         /*only if the mask has values add the get_attribute_all service */
    {
        InsertService( clazz, kGetAttributeAll, &GetAttributeAll, "GetAttributeAll" );  /* bind instance services to the class*/
    }

    InsertService( clazz, kGetAttributeSingle, &GetAttributeSingle,
            "GetAttributeSingle" );

    return clazz;
}


void InsertAttribute( CipInstance* instance, EipUint16 attribute_number,
        EipUint8 cip_type, void* data, EipByte cip_flags )
{
    CipAttributeStruct* attribute;

    attribute = instance->attributes;
    OPENER_ASSERT( NULL != attribute );

    /* adding a attribute to a class that was not declared to have any attributes is not allowed */
    for( int i = 0; i < instance->cip_class->number_of_attributes; i++ )
    {
        if( attribute->data == NULL ) /* found non set attribute */
        {
            attribute->attribute_number = attribute_number;
            attribute->type = cip_type;
            attribute->attribute_flags = cip_flags;
            attribute->data = data;

            if( attribute_number > instance->cip_class->highest_attribute_number ) /* remember the max attribute number that was defined*/
            {
                instance->cip_class->highest_attribute_number = attribute_number;
            }

            return;
        }

        attribute++;
    }

    OPENER_TRACE_ERR(
            "Tried to insert to many attributes into class: %" PRIu32 ", instance %" PRIu32 "\n",
            instance->cip_class->m_stSuper.instance_number,
            instance->instance_number );

    OPENER_ASSERT( 0 );
    /* trying to insert too many attributes*/
}


void InsertService( CipClass* clazz, EipUint8 service_number,
        CipServiceFunction service_function, char* service_name )
{
    CipServiceStruct* p = clazz->services;
    OPENER_ASSERT( p != 0 );

    /* adding a service to a class that was not declared to have services is not allowed*/
    for( int i = 0; i < clazz->number_of_services; i++ )                                /* Iterate over all service slots attached to the class */
    {
        if( p->service_number == service_number || p->service_function == NULL )    /* found undefined service slot*/
        {
            p->service_number = service_number;                                     /* fill in service number*/
            p->service_function = service_function;                                 /* fill in function address*/
            p->name = service_name;
            return;
        }

        p++;
    }

    OPENER_ASSERT( 0 );
    /* adding more services than were declared is a no-no*/
}


CipAttributeStruct* GetCipAttribute( CipInstance* instance,
        EipUint16 attribute_number )
{
    int i;
    CipAttributeStruct* attribute = instance->attributes; /* init pointer to array of attributes*/

    for( i = 0; i < instance->cip_class->number_of_attributes; i++ )
    {
        if( attribute_number == attribute->attribute_number )
            return attribute;
        else
            attribute++;
    }

    OPENER_TRACE_WARN( "attribute %d not defined\n", attribute_number );

    return 0;
}


/* TODO this needs to check for buffer overflow*/
EipStatus GetAttributeSingle( CipInstance* instance,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response )
{
    /* Mask for filtering get-ability */
    EipByte get_mask;

    CipAttributeStruct* attribute = GetCipAttribute(
            instance, request->request_path.attribute_number );

    EipByte* message = response->data;

    response->data_length = 0;
    response->reply_service = (0x80 | request->service);

    response->general_status = kCipErrorAttributeNotSupported;
    response->size_of_additional_status = 0;

    /* set filter according to service: get_attribute_all or get_attribute_single */
    if( kGetAttributeAll == request->service )
    {
        get_mask = kGetableAll;
        response->general_status = kCipErrorSuccess;
    }
    else
    {
        get_mask = kGetableSingle;
    }

    if( attribute && attribute->data )
    {
        if( attribute->attribute_flags & get_mask )
        {
            OPENER_TRACE_INFO( "%s: getAttribute %d\n",
                    __func__, request->request_path.attribute_number );

            /* create a reply message containing the data*/

            /* TODO think if it is better to put this code in an own
             * getAssemblyAttributeSingle functions which will call get attribute
             * single.
             */

            if( attribute->type == kCipByteArray
                && instance->cip_class->class_id == kCipAssemblyClassCode )
            {
                /* we are getting a byte array of a assembly object, kick out to the app callback */
                OPENER_TRACE_INFO( " -> getAttributeSingle CIP_BYTE_ARRAY\r\n" );
                BeforeAssemblyDataSend( instance );
            }

            response->data_length = EncodeData( attribute->type,
                    attribute->data,
                    &message );

            response->general_status = kCipErrorSuccess;
        }
    }

    return kEipStatusOkSend;
}


int EncodeData( EipUint8 cip_type, void* data, EipUint8** message )
{
    int counter = 0;

    switch( cip_type )
    /* check the data type of attribute */
    {
    case (kCipBool):
    case (kCipSint):
    case (kCipUsint):
    case (kCipByte):
        **message = *(EipUint8*) (data);
        ++(*message);
        counter = 1;
        break;

    case (kCipInt):
    case (kCipUint):
    case (kCipWord):
        AddIntToMessage( *(EipUint16*) (data), message );
        counter = 2;
        break;

    case (kCipDint):
    case (kCipUdint):
    case (kCipDword):
    case (kCipReal):
        AddDintToMessage( *(EipUint32*) (data), message );
        counter = 4;
        break;

#ifdef OPENER_SUPPORT_64BIT_DATATYPES
    case (kCipLint):
    case (kCipUlint):
    case (kCipLword):
    case (kCipLreal):
        AddLintToMessage( *(EipUint64*) (data), message );
        counter = 8;
        break;
#endif

    case (kCipStime):
    case (kCipDate):
    case (kCipTimeOfDay):
    case (kCipDateAndTime):
        break;

    case (kCipString):
        {
            CipString* string = (CipString*) data;

            AddIntToMessage( *(EipUint16*) &(string->length), message );
            memcpy( *message, string->string, string->length );
            *message += string->length;

            counter = string->length + 2; /* we have a two byte length field */

            if( counter & 0x01 )
            {
                /* we have an odd byte count */
                **message = 0;
                ++(*message);
                counter++;
            }
        }
        break;

    case (kCipString2):
    case (kCipFtime):
    case (kCipLtime):
    case (kCipItime):
    case (kCipStringN):
        break;

    case (kCipShortString):
        {
            CipShortString* short_string = (CipShortString*) data;

            **message = short_string->length;
            ++(*message);

            memcpy( *message, short_string->string, short_string->length );
            *message += short_string->length;

            counter = short_string->length + 1;
        }
        break;

    case (kCipTime):
        break;

    case (kCipEpath):
        counter = EncodeEPath( (CipEpath*) data, message );
        break;

    case (kCipEngUnit):
        break;

    case (kCipUsintUsint):
        {
            CipRevision* revision = (CipRevision*) data;

            **message = revision->major_revision;
            ++(*message);
            **message = revision->minor_revision;
            ++(*message);
            counter = 2;
        }
        break;

    case (kCipUdintUdintUdintUdintUdintString):
        {
            /* TCP/IP attribute 5 */
            CipTcpIpNetworkInterfaceConfiguration* tcp_ip_network_interface_configuration =
                (CipTcpIpNetworkInterfaceConfiguration*) data;
            AddDintToMessage(
                    ntohl( tcp_ip_network_interface_configuration->ip_address ), message );
            AddDintToMessage(
                    ntohl( tcp_ip_network_interface_configuration->network_mask ), message );
            AddDintToMessage( ntohl( tcp_ip_network_interface_configuration->gateway ),
                    message );
            AddDintToMessage(
                    ntohl( tcp_ip_network_interface_configuration->name_server ), message );
            AddDintToMessage(
                    ntohl( tcp_ip_network_interface_configuration->name_server_2 ),
                    message );
            counter = 20;
            counter += EncodeData(
                    kCipString, &(tcp_ip_network_interface_configuration->domain_name),
                    message );
        }
        break;

    case (kCip6Usint):
        {
            EipUint8* p = (EipUint8*) data;
            memcpy( *message, p, 6 );
            counter = 6;
        }
        break;

    case (kCipMemberList):
        break;

    case (kCipByteArray):
        {
            CipByteArray* cip_byte_array;
            OPENER_TRACE_INFO( " -> get attribute byte array\r\n" );
            cip_byte_array = (CipByteArray*) data;
            memcpy( *message, cip_byte_array->data, cip_byte_array->length );
            *message += cip_byte_array->length;
            counter = cip_byte_array->length;
        }
        break;

    case (kInternalUint6):    /* TODO for port class attribute 9, hopefully we can find a better way to do this*/
        {
            EipUint16* internal_unit16_6 = (EipUint16*) data;

            AddIntToMessage( internal_unit16_6[0], message );
            AddIntToMessage( internal_unit16_6[1], message );
            AddIntToMessage( internal_unit16_6[2], message );
            AddIntToMessage( internal_unit16_6[3], message );
            AddIntToMessage( internal_unit16_6[4], message );
            AddIntToMessage( internal_unit16_6[5], message );
            counter = 12;
        }
        break;

    default:
        break;
    }

    return counter;
}


int DecodeData( EipUint8 cip_type, void* data, EipUint8** message )
{
    int number_of_decoded_bytes = -1;

    switch( cip_type )
    /* check the data type of attribute */
    {
    case (kCipBool):
    case (kCipSint):
    case (kCipUsint):
    case (kCipByte):
        *(EipUint8*) (data) = **message;
        ++(*message);
        number_of_decoded_bytes = 1;
        break;

    case (kCipInt):
    case (kCipUint):
    case (kCipWord):
        ( *(EipUint16*) (data) ) = GetIntFromMessage( message );
        number_of_decoded_bytes = 2;
        break;

    case (kCipDint):
    case (kCipUdint):
    case (kCipDword):
        ( *(EipUint32*) (data) ) = GetDintFromMessage( message );
        number_of_decoded_bytes = 4;
        break;

#ifdef OPENER_SUPPORT_64BIT_DATATYPES
    case (kCipLint):
    case (kCipUlint):
    case (kCipLword):
        {
            ( *(EipUint64*) (data) ) = GetLintFromMessage( message );
            number_of_decoded_bytes = 8;
        }
        break;
#endif

    case (kCipString):
        {
            CipString* string = (CipString*) data;
            string->length = GetIntFromMessage( message );
            memcpy( string->string, *message, string->length );
            *message += string->length;

            number_of_decoded_bytes = string->length + 2; /* we have a two byte length field */

            if( number_of_decoded_bytes & 0x01 )
            {
                /* we have an odd byte count */
                ++(*message);
                number_of_decoded_bytes++;
            }
        }
        break;

    case (kCipShortString):
        {
            CipShortString* short_string = (CipShortString*) data;

            short_string->length = **message;
            ++(*message);

            memcpy( short_string->string, *message, short_string->length );
            *message += short_string->length;

            number_of_decoded_bytes = short_string->length + 1;
        }
        break;

    default:
        break;
    }

    return number_of_decoded_bytes;
}


EipStatus GetAttributeAll( CipInstance* instance,
        CipMessageRouterRequest* request,
        CipMessageRouterResponse* response )
{
    int i, j;
    EipUint8* reply;
    CipAttributeStruct* attribute;
    CipServiceStruct*   service;

    reply = response->data;      /* pointer into the reply */
    attribute = instance->attributes;           /* pointer to list of attributes*/
    service = instance->cip_class->services;    /* pointer to list of services*/

    if( instance->instance_number == 2 )
    {
        OPENER_TRACE_INFO( "GetAttributeAll: instance number 2\n" );
    }

    for( i = 0; i < instance->cip_class->number_of_services; i++ )  /* hunt for the GET_ATTRIBUTE_SINGLE service*/
    {
        if( service->service_number == kGetAttributeSingle )        /* found the service */
        {
            if( 0 == instance->cip_class->number_of_attributes )
            {
                response->data_length = 0; /*there are no attributes to be sent back*/
                response->reply_service = (0x80 | request->service);

                response->general_status = kCipErrorServiceNotSupported;
                response->size_of_additional_status = 0;
            }
            else
            {
                for( j = 0; j < instance->cip_class->number_of_attributes; j++ ) /* for each instance attribute of this class */
                {
                    int attrNum = attribute->attribute_number;

                    if( attrNum < 32
                        && (instance->cip_class->get_attribute_all_mask & 1 << attrNum) ) /* only return attributes that are flagged as being part of GetAttributeALl */
                    {
                        request->request_path.attribute_number = attrNum;

                        if( kEipStatusOkSend
                            != service->service_function( instance, request,
                                    response ) )
                        {
                            response->data = reply;
                            return kEipStatusError;
                        }

                        response->data += response->data_length;
                    }

                    attribute++;
                }

                response->data_length = response->data - reply;
                response->data = reply;
            }

            return kEipStatusOkSend;
        }

        service++;
    }

    return kEipStatusOk; /* Return kEipStatusOk if cannot find GET_ATTRIBUTE_SINGLE service*/
}


int EncodeEPath( CipEpath* epath, EipUint8** message )
{
    unsigned int length = epath->path_size;

    AddIntToMessage( epath->path_size, message );

    if( epath->class_id < 256 )
    {
        **message = 0x20; /*8Bit Class Id */
        ++(*message);
        **message = (EipUint8) epath->class_id;
        ++(*message);
        length -= 1;
    }
    else
    {
        **message = 0x21;   /*16Bit Class Id */
        ++(*message);
        **message = 0;      /*pad byte */
        ++(*message);
        AddIntToMessage( epath->class_id, message );
        length -= 2;
    }

    if( 0 < length )
    {
        if( epath->instance_number < 256 )
        {
            **message = 0x24; /*8Bit Instance Id */
            ++(*message);
            **message = (EipUint8) epath->instance_number;
            ++(*message);
            length -= 1;
        }
        else
        {
            **message = 0x25;   /*16Bit Instance Id */
            ++(*message);
            **message = 0;      /*padd byte */
            ++(*message);
            AddIntToMessage( epath->instance_number, message );
            length -= 2;
        }

        if( 0 < length )
        {
            if( epath->attribute_number < 256 )
            {
                **message = 0x30; /*8Bit Attribute Id */
                ++(*message);
                **message = (EipUint8) epath->attribute_number;
                ++(*message);
                length -= 1;
            }
            else
            {
                **message = 0x31;   /*16Bit Attribute Id */
                ++(*message);
                **message = 0;      /*pad byte */
                ++(*message);
                AddIntToMessage( epath->attribute_number, message );
                length -= 2;
            }
        }
    }

    return 2 + epath->path_size * 2; /* path size is in 16 bit chunks according to the specification */
}


int DecodePaddedEPath( CipEpath* epath, EipUint8** message )
{
    unsigned int number_of_decoded_elements;
    EipUint8* message_runner = *message;

    epath->path_size = *message_runner;
    message_runner++;

    /* copy path to structure, in version 0.1 only 8 bit for Class,Instance and Attribute, need to be replaced with function */
    epath->class_id = 0;
    epath->instance_number  = 0;
    epath->attribute_number = 0;

    for( number_of_decoded_elements = 0;
         number_of_decoded_elements < epath->path_size;
         number_of_decoded_elements++ )
    {
        if( kSegmentTypeSegmentTypeReserved ==
            ( (*message_runner) & kSegmentTypeSegmentTypeReserved ) )
        {
            /* If invalid/reserved segment type, segment type greater than 0xE0 */
            return kEipStatusError;
        }

        switch( *message_runner )
        {
        case kSegmentTypeLogicalSegment + kLogicalSegmentLogicalTypeClassId +
            kLogicalSegmentLogicalFormatEightBitValue:
            epath->class_id = *(EipUint8*) (message_runner + 1);
            message_runner  += 2;
            break;

        case kSegmentTypeLogicalSegment + kLogicalSegmentLogicalTypeClassId +
            kLogicalSegmentLogicalFormatSixteenBitValue:
            message_runner  += 2;
            epath->class_id = GetIntFromMessage( &(message_runner) );
            number_of_decoded_elements++;
            break;

        case kSegmentTypeLogicalSegment + kLogicalSegmentLogicalTypeInstanceId +
            kLogicalSegmentLogicalFormatEightBitValue:
            epath->instance_number = *(EipUint8*) (message_runner + 1);
            message_runner += 2;
            break;

        case kSegmentTypeLogicalSegment + kLogicalSegmentLogicalTypeInstanceId +
            kLogicalSegmentLogicalFormatSixteenBitValue:
            message_runner += 2;
            epath->instance_number = GetIntFromMessage( &(message_runner) );
            number_of_decoded_elements++;
            break;

        case kSegmentTypeLogicalSegment + kLogicalSegmentLogicalTypeAttributeId +
            kLogicalSegmentLogicalFormatEightBitValue:
            epath->attribute_number = *(EipUint8*) (message_runner + 1);
            message_runner += 2;
            break;

        case kSegmentTypeLogicalSegment + kLogicalSegmentLogicalTypeAttributeId +
            kLogicalSegmentLogicalFormatSixteenBitValue:
            message_runner += 2;
            epath->attribute_number = GetIntFromMessage( &(message_runner) );
            number_of_decoded_elements++;
            break;

        default:
            OPENER_TRACE_ERR( "wrong path requested\n" );
            return kEipStatusError;
            break;
        }
    }

    *message = message_runner;
    return number_of_decoded_elements * 2 + 1; /* i times 2 as every encoding uses 2 bytes */
}
