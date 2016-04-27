/*******************************************************************************
 * Copyright (c) 2009, Rockwell Automation, Inc.
 * All rights reserved.
 *
 ******************************************************************************/

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "endianconv.h"

OpenerEndianess g_opener_platform_endianess = kOpenerEndianessUnknown;

// THESE ROUTINES MODIFY THE BUFFER POINTER

/**
 *   @brief Reads EIP_UINT8 from *buffer and converts little endian to host.
 *   @param buffer pointer where data should be reed.
 *   @return EIP_UINT8 data value
 */
EipUint8 GetSintFromMessage( EipUint8** buffer )
{
    unsigned char* buffer_address = (unsigned char*) *buffer;
    EipUint16 data = buffer_address[0];

    *buffer += 1;
    return data;
}


// little-endian-to-host unsigned 16 bit

/**
 *   @brief Reads EIP_UINT16 from *buffer and converts little endian to host.
 *   @param buffer pointer where data should be reed.
 *   @return EIP_UINT16 data value
 */
EipUint16 GetIntFromMessage( EipUint8** buffer )
{
    unsigned char* buffer_address = (unsigned char*) *buffer;
    EipUint16 data = buffer_address[0] | buffer_address[1] << 8;

    *buffer += 2;
    return data;
}


/**
 *   @brief Reads EIP_UINT32 from *buffer and converts little endian to host.
 *   @param buffer pointer where data should be reed.
 *   @return EIP_UNÍT32 value
 */
EipUint32 GetDintFromMessage( EipUint8** buffer )
{
    unsigned char* p = (unsigned char*) *buffer;
    EipUint32 data = p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;

    *buffer += 4;
    return data;
}


/**
 * @brief converts UINT8 data from host to little endian an writes it to buffer.
 * @param data value to be written
 * @param buffer pointer where data should be written.
 */
int AddSintToMessage( EipUint8 data, EipUint8** buffer )
{
    unsigned char* p = (unsigned char*) *buffer;

    p[0] = (unsigned char) data;
    *buffer += 1;
    return 1;
}


/**
 * @brief converts UINT16 data from host to little endian an writes it to buffer.
 * @param data value to be written
 * @param buffer pointer where data should be written.
 */
int AddIntToMessage( EipUint16 data, EipUint8** buffer )
{
    unsigned char* p = (unsigned char*) *buffer;

    p[0] = (unsigned char) data;
    p[1] = (unsigned char) (data >> 8);
    *buffer += 2;
    return 2;
}


/**
 * @brief Converts UINT32 data from host to little endian and writes it to buffer.
 * @param data value to be written
 * @param buffer pointer where data should be written.
 */
int AddDintToMessage( EipUint32 data, EipUint8** buffer )
{
    unsigned char* p = (unsigned char*) *buffer;

    p[0] = (unsigned char) data;
    p[1] = (unsigned char) (data >> 8);
    p[2] = (unsigned char) (data >> 16);
    p[3] = (unsigned char) (data >> 24);
    *buffer += 4;

    return 4;
}


#ifdef CIPSTER_SUPPORT_64BIT_DATATYPES

/**
 *   @brief Reads EipUint64 from *pa_buf and converts little endian to host.
 *   @param pa_buf pointer where data should be reed.
 *   @return EipUint64 value
 */
EipUint64 GetLintFromMessage( EipUint8** buffer )
{
    EipUint8* buffer_address = *buffer;
    EipUint64 data = ( ( ( (EipUint64) buffer_address[0] ) << 56 )
                       & 0xFF00000000000000LL )
                     + ( ( ( (EipUint64) buffer_address[1] ) << 48 ) & 0x00FF000000000000LL )
                     + ( ( ( (EipUint64) buffer_address[2] ) << 40 ) & 0x0000FF0000000000LL )
                     + ( ( ( (EipUint64) buffer_address[3] ) << 32 ) & 0x000000FF00000000LL )
                     + ( ( ( (EipUint64) buffer_address[4] ) << 24 ) & 0x00000000FF000000 )
                     + ( ( ( (EipUint64) buffer_address[5] ) << 16 ) & 0x0000000000FF0000 )
                     + ( ( ( (EipUint64) buffer_address[6] ) << 8 ) & 0x000000000000FF00 )
                     + ( ( (EipUint64) buffer_address[7] ) & 0x00000000000000FF );

    (*buffer) += 8;
    return data;
}


/**
 * @brief Converts EipUint64 data from host to little endian and writes it to buffer.
 * @param data value to be written
 * @param buffer pointer where data should be written.
 */
int AddLintToMessage( EipUint64 data, EipUint8** buffer )
{
    EipUint8* buffer_address = *buffer;

    buffer_address[0] = (EipUint8) (data >> 56) & 0xFF;
    buffer_address[1] = (EipUint8) (data >> 48) & 0xFF;
    buffer_address[2] = (EipUint8) (data >> 40) & 0xFF;
    buffer_address[3] = (EipUint8) (data >> 32) & 0xFF;
    buffer_address[4] = (EipUint8) (data >> 24) & 0xFF;
    buffer_address[5] = (EipUint8) (data >> 16) & 0xFF;
    buffer_address[6] = (EipUint8) (data >> 8) & 0xFF;
    buffer_address[7] = (EipUint8) (data) & 0xFF;
    (*buffer) += 8;

    return 8;
}


#endif


int EncapsulateIpAddress( EipUint16 port, EipUint32 address,
        EipByte** communication_buffer )
{
    int size = 0;

    if( kCIPsterEndianessLittle == g_opener_platform_endianess )
    {
        size += AddIntToMessage( htons( AF_INET ), communication_buffer );
        size += AddIntToMessage( port, communication_buffer );
        size += AddDintToMessage( address, communication_buffer );
    }
    else
    {
        if( kCIPsterEndianessBig == g_opener_platform_endianess )
        {
            (*communication_buffer)[0]  = (unsigned char) (AF_INET >> 8);
            (*communication_buffer)[1]  = (unsigned char) AF_INET;
            *communication_buffer += 2;
            size += 2;

            (*communication_buffer)[0]  = (unsigned char) (port >> 8);
            (*communication_buffer)[1]  = (unsigned char) port;
            *communication_buffer += 2;
            size += 2;

            (*communication_buffer)[3]  = (unsigned char) address;
            (*communication_buffer)[2]  = (unsigned char) (address >> 8);
            (*communication_buffer)[1]  = (unsigned char) (address >> 16);
            (*communication_buffer)[0]  = (unsigned char) (address >> 24);
            *communication_buffer += 4;
            size += 4;
        }
        else
        {
            fprintf( stderr,
                    "No endianess detected! Probably the DetermineEndianess function was not executed!" );
            exit( EXIT_FAILURE );
        }
    }

    return size;
}


/**
 * @brief Detects Endianess of the platform and sets global g_nCIPsterPlatformEndianess variable accordingly
 *
 * Detects Endianess of the platform and sets global variable g_nCIPsterPlatformEndianess accordingly,
 * whereas 0 equals little endian and 1 equals big endian
 */
void DetermineEndianess()
{
    int i = 1;
    char* p = (char*) &i;

    if( p[0] == 1 )
    {
        g_opener_platform_endianess = kCIPsterEndianessLittle;
    }
    else
    {
        g_opener_platform_endianess = kCIPsterEndianessBig;
    }
}


/**
 * @brief Returns global variable g_nCIPsterPlatformEndianess, whereas 0 equals little endian and 1 equals big endian
 *
 * @return 0 equals little endian and 1 equals big endian
 */
int GetEndianess()
{
    return g_opener_platform_endianess;
}


void MoveMessageNOctets( CipOctet** message_runner, int n )
{
    *message_runner += n;
}


int FillNextNMessageOctetsWithValueAndMoveToNextPosition( EipByte value,
        int count, CipOctet** message_runner )
{
    memset( *message_runner, value, count );
    *message_runner += count;
    return count;
}
