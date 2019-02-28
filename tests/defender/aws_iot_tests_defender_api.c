/*
 * Copyright (C) 2018 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Standard includes. */
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>

/* Network includes. */
#include <netdb.h>
#include <arpa/inet.h>

/* Defender internal includes. */
#include "private/aws_iot_defender_internal.h"

/* Serializer includes. */
#include "iot_serializer.h"

#include "cbor.h"

/* Metrics includes. */
#include "iot_metrics.h"

#include "unity_fixture.h"

/* Time interval to wait for a state to be true. */
#define _WAIT_STATE_INTERVAL_SECONDS    1

/* Total time to wait for a state to be true. */
#define _WAIT_STATE_TOTAL_SECONDS       5

/* Time interval for defender agent to publish metrics. It will be throttled if too frequent. */
/* TODO: if we can change "thingname" in each test, this can be lowered. */
#define _DEFENDER_PUBLISH_INTERVAL_SECONDS    15

/* Estimated max size of message payload received in MQTT callback. */
#define _PAYLOAD_MAX_SIZE                     200

/* Estimated max size of metrics report defender published. */
#define _METRICS_MAX_SIZE                     200

/* Max size of address: IP + port. */
#define _MAX_ADDRESS_LENGTH                   25

/* Use a big number to represent no event happened in defender. */
#define _NO_EVENT                             10000

/* Define a decoder based on chosen format. */
#if AWS_IOT_DEFENDER_FORMAT == AWS_IOT_DEFENDER_FORMAT_CBOR

    #define _Decoder    _IotSerializerCborDecoder /**< Global defined in iot_serializer.h . */

#elif AWS_IOT_DEFENDER_FORMAT == AWS_IOT_DEFENDER_FORMAT_JSON

    #define _Decoder    _IotSerializerJsonDecoder /**< Global defined in iot_serializer.h . */

#endif

/* Empty callback structure passed to startInfo. */
static const AwsIotDefenderCallback_t _EMPTY_CALLBACK = { .function = NULL, .param1 = NULL };

/*------------------ global variables -----------------------------*/

static uint8_t _payloadBuffer[ _PAYLOAD_MAX_SIZE ];
static uint8_t _metricsBuffer[ _METRICS_MAX_SIZE ];

static AwsIotDefenderCallback_t _testCallback;

static AwsIotDefenderStartInfo_t _startInfo = AWS_IOT_DEFENDER_START_INFO_INITIALIZER;

static AwsIotDefenderCallbackInfo_t _callbackInfo;

static IotSerializerDecoderObject_t _decoderObject;
static IotSerializerDecoderObject_t _metricsObject;

/*------------------ Functions -----------------------------*/

/* Copy data from MQTT callback to local buffer. */
static void _copyDataCallbackFunction( void * param1,
                                       AwsIotDefenderCallbackInfo_t * const pCallbackInfo );

static void _waitForAnyEvent( uint32_t timeoutSec );

static void _assertEvent( AwsIotDefenderEventType_t event,
                          uint32_t timeoutSec );

/* Wait for metrics to be accepted by defender service, for maxinum timeout. */
static void _waitForMetricsAccepted( uint32_t timeoutSec );

/* Verify common section of metrics report. */
static void _verifyMetricsCommon();

/* Verify tcp connections in metrics report. */
static void _verifyTcpConections( int total,
                                  ... );

/* Indicate this test doesn't actually publish report. */
static void _publishMetricsNotNeeded();

static void _resetCalbackInfo();

static char * _getIotAddress();

TEST_GROUP( Full_DEFENDER );

TEST_SETUP( Full_DEFENDER )
{
    _resetCalbackInfo();

    _decoderObject = ( IotSerializerDecoderObject_t ) IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;
    _metricsObject = ( IotSerializerDecoderObject_t ) IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;

    /* Reset test callback. */
    _testCallback = ( AwsIotDefenderCallback_t ) {
        .function = _copyDataCallbackFunction, .param1 = NULL
    };

    /* Setup startInfo. */
    _startInfo.serverInfo.pHostName = IOT_TEST_SERVER;
    _startInfo.serverInfo.port = IOT_TEST_PORT;
    _startInfo.pThingName = AWS_IOT_TEST_SHADOW_THING_NAME;
    _startInfo.thingNameLength = strlen( AWS_IOT_TEST_SHADOW_THING_NAME );
    _startInfo.callback = _EMPTY_CALLBACK;

    /* Setup TLS information. */
    _startInfo.credentials = ( IotNetworkCredentialsOpenssl_t ) IOT_TEST_NETWORK_CREDENTIALS_INITIALIZER;
}

TEST_TEAR_DOWN( Full_DEFENDER )
{
    AwsIotDefender_Stop();

    /* Actually get defender callback. */
    if( ( _callbackInfo.eventType == AWS_IOT_DEFENDER_METRICS_ACCEPTED ) ||
        ( _callbackInfo.eventType == AWS_IOT_DEFENDER_METRICS_REJECTED ) )
    {
        sleep( _DEFENDER_PUBLISH_INTERVAL_SECONDS );
    }
}

TEST_GROUP_RUNNER( Full_DEFENDER )
{
    /*
     * Setup: none
     * Action: call Start API with invliad IoT endpoint
     * Expectation: Start API returns network connection failure
     */
    RUN_TEST_CASE( Full_DEFENDER, Start_with_wrong_network_information );

    /*
     * Setup: defender not started yet
     * Action: call SetMetrics API with an invalid big integer as metrics group
     * Expectation:
     * - SetMetrics API return invalid input
     * - global metrics flag array are untouched
     */
    RUN_TEST_CASE( Full_DEFENDER, SetMetrics_with_invalid_metrics_group );

    /*
     * Setup: defender not started yet
     * Action: call SetMetrics API with Tcp connections group and "All Metrics" flag value
     * Expectation:
     * - SetMetrics API return success
     * - global metrics flag array are updated correctly
     */
    RUN_TEST_CASE( Full_DEFENDER, SetMetrics_with_TCP_connections_all );

    /*
     * Setup: defender is started
     * Action: call SetMetrics API with Tcp connections group and "All Metrics" flag value
     * Expectation:
     * - SetMetrics API return success
     * - global metrics flag array are updated correctly
     */
    RUN_TEST_CASE( Full_DEFENDER, SetMetrics_after_defender_started );

    /*
     * Setup: defender not started yet
     * Action: call SetPeriod API with small value less than 300
     * Expectation:
     * - SetPeriod API return "period too short" error
     */
    /*RUN_TEST_CASE( Full_DEFENDER, SetPeriod_too_short ); */

    /*
     * Setup: defender not started yet
     * Action: call SetPeriod API with 301
     * Expectation:
     * - SetPeriod API return success
     */
    RUN_TEST_CASE( Full_DEFENDER, SetPeriod_with_proper_value );

    /*
     * Setup: defender is started
     * Action: call SetPeriod API with 600
     * Expectation:
     * - SetPeriod API return success
     */
    RUN_TEST_CASE( Full_DEFENDER, SetPeriod_after_started );

    /*
     * Setup: kept from publishing metrics report
     * Action: call Start API with correct network information
     * Expectation: Start API return success
     */
    RUN_TEST_CASE( Full_DEFENDER, Start_should_return_success );

    /*
     * Setup: call Start API the first time; kept from publishing metrics report
     * Action: call Start API second time
     * Expectation: Start API return "already started" error
     */
    RUN_TEST_CASE( Full_DEFENDER, Start_should_return_err_if_already_started );

    /*
     * Setup: not set any metrics; register test callback
     * Action: call Start API
     * Expectation: metrics are accepted by defender service
     */
    RUN_TEST_CASE( Full_DEFENDER, Metrics_empty_are_published );

    /*
     * Setup: set "tcp connections" with "all metrics"; register test callback
     * Action: call Start API
     * Expectation:
     * - metrics are accepted by defender service
     * - verify metrics report has correct content
     */
    RUN_TEST_CASE( Full_DEFENDER, Metrics_TCP_connections_all_are_published );

    /*
     * Setup: set "tcp connections" with "total count"; register test callback
     * Action: call Start API
     * Expectation:
     * - metrics are accepted by defender service
     * - verify metrics report has correct content
     */
    RUN_TEST_CASE( Full_DEFENDER, Metrics_TCP_connections_total_are_published );

    /*
     * Setup: set "tcp connections" with "remote address"; register test callback
     * Action: call Start API
     * Expectation:
     * - metrics are accepted by defender service
     * - verify metrics report has correct content
     */
    RUN_TEST_CASE( Full_DEFENDER, Metrics_TCP_connections_remote_addr_are_published );

    /*
     * Setup: set "tcp connections" with "total count"; register test callback; call Start API
     * Action: call Stop API; set "tcp connections" with "all metrics"; call Start again
     * Expectation:
     * - metrics are accepted by defender service in both times
     * - verify metrics report has correct content respectively in both times
     */
    RUN_TEST_CASE( Full_DEFENDER, Restart_and_updated_metrics_are_published );
}

TEST( Full_DEFENDER, SetMetrics_with_invalid_metrics_group )
{
    uint8_t i = 0;

    /* Input a dummy, invalid metrics group. */
    AwsIotDefenderError_t error = AwsIotDefender_SetMetrics( 10000,
                                                             AWS_IOT_DEFENDER_METRICS_ALL );

    /* SetMetrics should return "invalid input". */
    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_INVALID_INPUT, error );

    /* Assert metrics flag in each metrics group remains 0. */
    for( i = 0; i < _DEFENDER_METRICS_GROUP_COUNT; i++ )
    {
        TEST_ASSERT_EQUAL( 0, _AwsIotDefenderMetrics.metricsFlag[ i ] );
    }
}

TEST( Full_DEFENDER, SetMetrics_with_TCP_connections_all )
{
    /* Set "all metrics" for TCP connections metrics group. */
    AwsIotDefenderError_t error = AwsIotDefender_SetMetrics( AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS,
                                                             AWS_IOT_DEFENDER_METRICS_ALL );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_METRICS_ALL, _AwsIotDefenderMetrics.metricsFlag[ AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS ] );
}

TEST( Full_DEFENDER, SetMetrics_after_defender_started )
{
    _publishMetricsNotNeeded();

    AwsIotDefenderError_t error = AwsIotDefender_Start( &_startInfo );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    /* Set "all metrics" for TCP connections metrics group. */
    error = AwsIotDefender_SetMetrics( AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS,
                                       AWS_IOT_DEFENDER_METRICS_ALL );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_METRICS_ALL, _AwsIotDefenderMetrics.metricsFlag[ AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS ] );
}

TEST( Full_DEFENDER, Start_with_wrong_network_information )
{
    _publishMetricsNotNeeded();

    /* Set test callback to verify report. */
    _startInfo.callback = _testCallback;

    AwsIotDefenderError_t error = AwsIotDefender_Start( &_startInfo );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    _assertEvent( AWS_IOT_DEFENDER_NETWORK_CONNECTION_FAILED, _WAIT_STATE_TOTAL_SECONDS );
}

TEST( Full_DEFENDER, Start_should_return_success )
{
    _publishMetricsNotNeeded();

    AwsIotDefenderError_t error = AwsIotDefender_Start( &_startInfo );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );
}

TEST( Full_DEFENDER, Start_should_return_err_if_already_started )
{
    _publishMetricsNotNeeded();

    AwsIotDefenderError_t error = AwsIotDefender_Start( &_startInfo );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    /* Start defender for a second time. */
    error = AwsIotDefender_Start( &_startInfo );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_ALREADY_STARTED, error );
}

TEST( Full_DEFENDER, Metrics_empty_are_published )
{
    AwsIotDefenderError_t error;

    /* Set test callback to verify report. */
    _startInfo.callback = _testCallback;

    /* Start defender. */
    error = AwsIotDefender_Start( &_startInfo );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    _waitForMetricsAccepted( _WAIT_STATE_TOTAL_SECONDS );

    _verifyMetricsCommon();
    _verifyTcpConections( 0 );
}

TEST( Full_DEFENDER, Metrics_TCP_connections_all_are_published )
{
    AwsIotDefenderError_t error;

    /* Set "all metrics" for TCP connections metrics group. */
    error = AwsIotDefender_SetMetrics( AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS,
                                       AWS_IOT_DEFENDER_METRICS_ALL );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    /* Set test callback to verify report. */
    _startInfo.callback = _testCallback;

    /* Get Iot address from DNS. */
    char * pIotAddress = _getIotAddress();

    /* Start defender. */
    error = AwsIotDefender_Start( &_startInfo );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    /* Wait certain time for _reportAccepted to be true. */
    _waitForMetricsAccepted( _WAIT_STATE_TOTAL_SECONDS );

    _verifyMetricsCommon();
    _verifyTcpConections( 1, pIotAddress );
}

TEST( Full_DEFENDER, Metrics_TCP_connections_total_are_published )
{
    AwsIotDefenderError_t error;

    /* Set "total count" for TCP connections metrics group. */
    error = AwsIotDefender_SetMetrics( AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS,
                                       AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS_ESTABLISHED_TOTAL );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    /* Set test callback to verify report. */
    _startInfo.callback = _testCallback;

    /* Start defender. */
    error = AwsIotDefender_Start( &_startInfo );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    /* Wait certain time for _reportAccepted to be true. */
    _waitForMetricsAccepted( _WAIT_STATE_TOTAL_SECONDS );

    _verifyMetricsCommon();
    _verifyTcpConections( 1 );
}

TEST( Full_DEFENDER, Metrics_TCP_connections_remote_addr_are_published )
{
    AwsIotDefenderError_t error;

    /* Set "remote address" for TCP connections metrics group. */
    error = AwsIotDefender_SetMetrics( AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS,
                                       AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS_ESTABLISHED_REMOTE_ADDR );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    /* Set test callback to verify report. */
    _startInfo.callback = _testCallback;

    /* Get Iot address from DNS. */
    char * pIotAddress = _getIotAddress();

    /* Start defender. */
    error = AwsIotDefender_Start( &_startInfo );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, error );

    /* Wait certain time for _reportAccepted to be true. */
    _waitForMetricsAccepted( _WAIT_STATE_TOTAL_SECONDS );

    _verifyMetricsCommon();
    _verifyTcpConections( 1, pIotAddress );
}

TEST( Full_DEFENDER, Restart_and_updated_metrics_are_published )
{
    char * pIotAddress = NULL;

    /* Set "total count" for TCP connections metrics group. */
    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS,
                       AwsIotDefender_SetMetrics( AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS, AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS_ESTABLISHED_TOTAL ) );

    /* Set test callback to verify report. */
    _startInfo.callback = _testCallback;

    pIotAddress = _getIotAddress();

    /* Start defender. */
    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, AwsIotDefender_Start( &_startInfo ) );

    /* Wait certain time for _reportAccepted to be true. */
    _waitForMetricsAccepted( _WAIT_STATE_TOTAL_SECONDS );

    _verifyMetricsCommon();
    _verifyTcpConections( 1, pIotAddress );

    AwsIotDefender_Stop();

    /* Reset _callbackInfo before restarting. */
    _resetCalbackInfo();

    sleep( _DEFENDER_PUBLISH_INTERVAL_SECONDS );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS,
                       AwsIotDefender_SetMetrics( AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS, AWS_IOT_DEFENDER_METRICS_ALL ) );

    pIotAddress = _getIotAddress();

    /* Restart defender. */
    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, AwsIotDefender_Start( &_startInfo ) );

    /* Wait certain time for _reportAccepted to be true. */
    _waitForMetricsAccepted( _WAIT_STATE_TOTAL_SECONDS );

    _verifyMetricsCommon();
    _verifyTcpConections( 1, pIotAddress );
}

TEST( Full_DEFENDER, SetPeriod_too_short )
{
    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_PERIOD_TOO_SHORT, AwsIotDefender_SetPeriod( 299 ) );
}

TEST( Full_DEFENDER, SetPeriod_with_proper_value )
{
    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, AwsIotDefender_SetPeriod( 301 ) );

    TEST_ASSERT_EQUAL( 301, AwsIotDefender_GetPeriod() );
}

TEST( Full_DEFENDER, SetPeriod_after_started )
{
    _publishMetricsNotNeeded();

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS,
                       AwsIotDefender_Start( &_startInfo ) );

    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_SUCCESS, AwsIotDefender_SetPeriod( 600 ) );

    TEST_ASSERT_EQUAL( 600, AwsIotDefender_GetPeriod() );
}

/*-----------------------------------------------------------*/

static void _copyDataCallbackFunction( void * param1,
                                       AwsIotDefenderCallbackInfo_t * const pCallbackInfo )
{
    /* Silence the compiler. */
    ( void ) param1;

    /* Print out rejected message to stdout. */
    if( pCallbackInfo->eventType == AWS_IOT_DEFENDER_METRICS_REJECTED )
    {
        CborParser cborParser;
        CborValue cborValue;
        cbor_parser_init( pCallbackInfo->pPayload, pCallbackInfo->payloadLength, 0, &cborParser, &cborValue );
        cbor_value_to_pretty( stdout, &cborValue );
    }

    /* Copy data from pCallbackInfo to _callbackInfo. */
    if( pCallbackInfo != NULL )
    {
        _callbackInfo.eventType = pCallbackInfo->eventType;
        _callbackInfo.metricsReportLength = pCallbackInfo->metricsReportLength;
        _callbackInfo.payloadLength = pCallbackInfo->payloadLength;

        if( _callbackInfo.payloadLength > 0 )
        {
            memcpy( ( uint8_t * ) _callbackInfo.pPayload, pCallbackInfo->pPayload, _callbackInfo.payloadLength );
        }

        if( _callbackInfo.metricsReportLength > 0 )
        {
            memcpy( ( uint8_t * ) _callbackInfo.pMetricsReport, pCallbackInfo->pMetricsReport, _callbackInfo.metricsReportLength );
        }
    }
}

/*-----------------------------------------------------------*/

static void _publishMetricsNotNeeded()
{
    /*_startInfo.pThingName = "dummy-thing-for-test"; */
    /*_startInfo.thingNameLength = ( uint16_t ) strlen( "dummy-thing-for-test" ); */

    /* Given a dummy IoT endpoint to fail network connection. */
    _startInfo.serverInfo.pHostName = "dummy endpoint";
}

/*-----------------------------------------------------------*/

static void _resetCalbackInfo()
{
    /* Clean data buffer. */
    memset( _payloadBuffer, 0, _PAYLOAD_MAX_SIZE );
    memset( _metricsBuffer, 0, _METRICS_MAX_SIZE );

    /* Reset callback info. */
    _callbackInfo = ( AwsIotDefenderCallbackInfo_t ) {
        .pMetricsReport = _metricsBuffer,
        .metricsReportLength = 0,
        .pPayload = _payloadBuffer,
        .payloadLength = 0,
        .eventType = _NO_EVENT
    };
}

/*-----------------------------------------------------------*/

static void _waitForAnyEvent( uint32_t timeoutSec )
{
    uint32_t maxIterations = timeoutSec / _WAIT_STATE_INTERVAL_SECONDS;
    uint32_t iter = 1;

    /* Wait for an event type to be set. */
    while( _callbackInfo.eventType == _NO_EVENT )
    {
        if( iter > maxIterations )
        {
            /* Timeout. */
            TEST_FAIL_MESSAGE( "No event has happened after max timeout." );
        }

        sleep( _WAIT_STATE_INTERVAL_SECONDS );

        iter++;
    }
}

/*-----------------------------------------------------------*/

static void _assertEvent( AwsIotDefenderEventType_t event,
                          uint32_t timeoutSec )
{
    _waitForAnyEvent( timeoutSec );

    TEST_ASSERT_EQUAL( event, _callbackInfo.eventType );
}

/*-----------------------------------------------------------*/

/* Assert the cause of rejection is throttle. */
static void _assertRejectDueToThrottle()
{
    TEST_ASSERT_NOT_NULL( _callbackInfo.pPayload );
    TEST_ASSERT_GREATER_THAN( 0, _callbackInfo.payloadLength );

    IotSerializerDecoderObject_t decoderObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;
    IotSerializerDecoderObject_t statusDetailsObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;
    IotSerializerDecoderObject_t errorCodeObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;

    char errorCode[ 12 ] = "";

    IotSerializerError_t error = _Decoder.init( &decoderObject, _callbackInfo.pPayload, _callbackInfo.payloadLength );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_CONTAINER_MAP, decoderObject.type );

    error = _Decoder.find( &decoderObject, "statusDetails", &statusDetailsObject );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_CONTAINER_MAP, statusDetailsObject.type );

    errorCodeObject.value.pString = ( uint8_t * ) errorCode;
    errorCodeObject.value.stringLength = 12;

    error = _Decoder.find( &statusDetailsObject, "ErrorCode", &errorCodeObject );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SCALAR_TEXT_STRING, errorCodeObject.type );

    TEST_ASSERT_EQUAL( 0, strncmp( ( const char * ) errorCodeObject.value.pString, "Throttled", errorCodeObject.value.stringLength ) );

    _Decoder.destroy( &statusDetailsObject );
    _Decoder.destroy( &decoderObject );
}

/*-----------------------------------------------------------*/

static void _waitForMetricsAccepted( uint32_t timeoutSec )
{
    _waitForAnyEvent( timeoutSec );

    if( _callbackInfo.eventType == AWS_IOT_DEFENDER_METRICS_REJECTED )
    {
        _assertRejectDueToThrottle();

        return;
    }

    /* Assert metrics is accepted. */
    TEST_ASSERT_EQUAL( AWS_IOT_DEFENDER_METRICS_ACCEPTED, _callbackInfo.eventType );

    TEST_ASSERT_NOT_NULL( _callbackInfo.pPayload );
    TEST_ASSERT_GREATER_THAN( 0, _callbackInfo.payloadLength );

    IotSerializerDecoderObject_t decoderObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;

    IotSerializerError_t error = _Decoder.init( &decoderObject, _callbackInfo.pPayload, _callbackInfo.payloadLength );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_CONTAINER_MAP, decoderObject.type );

    IotSerializerDecoderObject_t statusObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;

    char status[ 10 ] = "";
    statusObject.value.pString = ( uint8_t * ) status;
    statusObject.value.stringLength = 10;

    error = _Decoder.find( &decoderObject, "status", &statusObject );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SCALAR_TEXT_STRING, statusObject.type );

    TEST_ASSERT_EQUAL( 0, strncmp( ( const char * ) statusObject.value.pString, "ACCEPTED", statusObject.value.stringLength ) );

    _Decoder.destroy( &statusObject );
    _Decoder.destroy( &decoderObject );
}

/*-----------------------------------------------------------*/

static void _verifyMetricsCommon()
{
    TEST_ASSERT_NOT_NULL( _callbackInfo.pMetricsReport );
    TEST_ASSERT_GREATER_THAN( 0, _callbackInfo.metricsReportLength );

    IotSerializerError_t error = _Decoder.init( &_decoderObject, _callbackInfo.pMetricsReport, _callbackInfo.metricsReportLength );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_CONTAINER_MAP, _decoderObject.type );

    error = _Decoder.find( &_decoderObject, "metrics", &_metricsObject );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

    TEST_ASSERT_EQUAL( IOT_SERIALIZER_CONTAINER_MAP, _metricsObject.type );
}

/*-----------------------------------------------------------*/

static void _verifyTcpConections( int total,
                                  ... )
{
    uint8_t i = 0;

    uint32_t tcpConnFlag = _AwsIotDefenderMetrics.metricsFlag[ AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS ];

    /* Assert find a "tcp_connections" map in "metrics" */
    IotSerializerDecoderObject_t tcpConnObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;

    IotSerializerError_t error = _Decoder.find( &_metricsObject, "tcp_connections", &tcpConnObject );

    /* If any TCP connections flag is specified. */
    if( tcpConnFlag & AWS_IOT_DEFENDER_METRICS_ALL )
    {
        /* Assert found the "tcp_connections" map. */
        TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

        TEST_ASSERT_EQUAL( IOT_SERIALIZER_CONTAINER_MAP, tcpConnObject.type );

        IotSerializerDecoderObject_t estConnObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;

        error = _Decoder.find( &tcpConnObject, "established_connections", &estConnObject );

        if( tcpConnFlag & AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS_ESTABLISHED )
        {
            /* Assert found a "established_connections" map in "tcp_connections" */
            TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

            TEST_ASSERT_EQUAL( IOT_SERIALIZER_CONTAINER_MAP, estConnObject.type );

            IotSerializerDecoderObject_t totalObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;

            error = _Decoder.find( &estConnObject, "total", &totalObject );

            if( tcpConnFlag & AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS_ESTABLISHED_TOTAL )
            {
                /* Assert find a "total" integer with value 1 in "established_connections" */
                TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

                TEST_ASSERT_EQUAL( IOT_SERIALIZER_SCALAR_SIGNED_INT, totalObject.type );

                TEST_ASSERT_EQUAL( total, totalObject.value.signedInt );
            }
            else
            {
                /* Assert not found the "total". */
                TEST_ASSERT_EQUAL( IOT_SERIALIZER_NOT_FOUND, error );
            }

            IotSerializerDecoderObject_t connsObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;
            IotSerializerDecoderIterator_t connIterator = IOT_SERIALIZER_DECODER_ITERATOR_INITIALIZER;

            error = _Decoder.find( &estConnObject, "connections", &connsObject );

            if( tcpConnFlag & AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS_ESTABLISHED_CONNECTIONS )
            {
                /* Assert find a "connections" array in "established_connections" */
                TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );
                TEST_ASSERT_EQUAL( IOT_SERIALIZER_CONTAINER_ARRAY, connsObject.type );

                error = _Decoder.stepIn( &connsObject, &connIterator );
                TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

                /* Create argument list for expected remote addresses. */
                va_list valist;
                va_start( valist, total );

                for( i = 0; i < total; i++ )
                {
                    /* Assert find one "connection" map in "connections" */
                    IotSerializerDecoderObject_t connMap = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;
                    error = _Decoder.get( connIterator, &connMap );
                    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );
                    TEST_ASSERT_EQUAL( IOT_SERIALIZER_CONTAINER_MAP, connMap.type );

                    IotSerializerDecoderObject_t remoteAddrObject = IOT_SERIALIZER_DECODER_OBJECT_INITIALIZER;

                    error = _Decoder.find( &connMap, "remote_addr", &remoteAddrObject );

                    if( tcpConnFlag & AWS_IOT_DEFENDER_METRICS_TCP_CONNECTIONS_ESTABLISHED_REMOTE_ADDR )
                    {
                        /* Assert find a "remote_addr" string in "connection" */
                        TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );

                        TEST_ASSERT_EQUAL( IOT_SERIALIZER_SCALAR_TEXT_STRING, remoteAddrObject.type );

                        /* Verify the passed address matching. */
                        TEST_ASSERT_EQUAL_STRING_LEN( va_arg( valist, char * ),
                                                      remoteAddrObject.value.pString,
                                                      remoteAddrObject.value.stringLength );
                    }
                    else
                    {
                        /* Assert not found the "remote_addr". */
                        TEST_ASSERT_EQUAL( IOT_SERIALIZER_NOT_FOUND, error );
                    }

                    error = _Decoder.next( connIterator );
                    TEST_ASSERT_EQUAL( IOT_SERIALIZER_SUCCESS, error );
                }

                va_end( valist );

                TEST_ASSERT_TRUE( _Decoder.isEndOfContainer( connIterator ) );

                _Decoder.stepOut( connIterator, &connsObject );
            }
            else
            {
                /* Assert not found the "connections". */
                TEST_ASSERT_EQUAL( IOT_SERIALIZER_NOT_FOUND, error );
            }

            _Decoder.destroy( &connsObject );
        }
        else
        {
            /* Assert not found the "established_connections" map. */
            TEST_ASSERT_EQUAL( IOT_SERIALIZER_NOT_FOUND, error );
        }

        _Decoder.destroy( &estConnObject );
    }
    else
    {
        /* Assert not found the "tcp_connections" map. */
        TEST_ASSERT_EQUAL( IOT_SERIALIZER_NOT_FOUND, error );
    }

    _Decoder.destroy( &tcpConnObject );
    _Decoder.destroy( &_metricsObject );
    _Decoder.destroy( &_decoderObject );
}

/*-----------------------------------------------------------*/

static char * _getIotAddress()
{
    static char iotAddress[ _MAX_ADDRESS_LENGTH ];

    struct addrinfo * pListHead = NULL;
    char * pIotAddressIp = NULL;

    /* Query DNS to get all the records. */
    getaddrinfo( IOT_TEST_SERVER, NULL, NULL, &pListHead );

    /* Convert the first record to string format of IP. */
    pIotAddressIp = inet_ntoa( ( ( struct sockaddr_in * ) pListHead->ai_addr )->sin_addr );

    sprintf( iotAddress, "%s:%d", pIotAddressIp, IOT_TEST_PORT );

    return iotAddress;
}