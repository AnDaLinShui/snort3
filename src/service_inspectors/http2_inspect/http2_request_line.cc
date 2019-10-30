//--------------------------------------------------------------------------
// Copyright (C) 2019-2019 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// http2_request_line.cc author Katura Harvey <katharve@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "http2_request_line.h"

#include <string.h>
#include <cstdlib>

#include "service_inspectors/http_inspect/http_common.h"
#include "service_inspectors/http_inspect/http_field.h"

#include "http2_enum.h"
#include "http2_flow_data.h"

using namespace HttpCommon;
using namespace Http2Enums;

const char* Http2RequestLine::AUTHORITY_NAME = ":authority";
const char* Http2RequestLine::METHOD_NAME = ":method";
const char* Http2RequestLine::PATH_NAME = ":path";
const char* Http2RequestLine::SCHEME_NAME = ":scheme";
const char* Http2RequestLine::OPTIONS = "OPTIONS";
const char* Http2RequestLine::CONNECT = "CONNECT";

Http2RequestLine::Http2RequestLine(Http2FlowData* _session_data, SourceId _source_id)
    : Http2StartLine(_session_data, _source_id)
{ }

Http2RequestLine::~Http2RequestLine()
{ }

bool Http2RequestLine::process_pseudo_header_name(const uint64_t index)
{
    if (!process_pseudo_header_precheck())
        return false;

    if (index <= AUTHORITY and authority.length() <= 0)
        value_coming = AUTHORITY;
    else if (index <= METHOD and method.length() <= 0)
        value_coming = METHOD;
    else if (index <= PATH and path.length() <= 0)
        value_coming = PATH;
    else if (index <= SCHEME and scheme.length() <= 0)
        value_coming = SCHEME;
    else
    {
        *session_data->infractions[source_id] += INF_INVALID_PSEUDO_HEADER;
        session_data->events[source_id]->create_event(EVENT_MISFORMATTED_HTTP2);
        return false;
    }
    return true;
}

bool Http2RequestLine::process_pseudo_header_name(const uint8_t* const& name, uint32_t length)
{
    if (!process_pseudo_header_precheck())
        return false;

    if (length == AUTHORITY_NAME_LENGTH and memcmp(name, AUTHORITY_NAME, length) == 0 and
            authority.length() <= 0)
        value_coming = AUTHORITY;
    else if (length == METHOD_NAME_LENGTH and memcmp(name, METHOD_NAME, length) == 0 and
            method.length() <= 0)
        value_coming = METHOD;
    else if (length == PATH_NAME_LENGTH and memcmp(name, PATH_NAME, length) == 0 and
            path.length() <= 0)
        value_coming = PATH;
    else if (length == SCHEME_NAME_LENGTH and memcmp(name, SCHEME_NAME, length) == 0 and
            scheme.length() <= 0)
        value_coming = SCHEME;
    else
    {
        *session_data->infractions[source_id] += INF_INVALID_PSEUDO_HEADER;
        session_data->events[source_id]->create_event(EVENT_MISFORMATTED_HTTP2);
        return false;
    }
    return true;
}

void Http2RequestLine::process_pseudo_header_value(const uint8_t* const& value, const uint32_t length)
{
    switch (value_coming)
    {
        case AUTHORITY:
            authority.set(length, value);
            break;
        case METHOD:
            method.set(length, value);
            break;
        case PATH:
            path.set(length, value);
            break;
        case SCHEME:
            scheme.set(length, value);
            break;
        default:
            // should never get here
            assert(false);
    }
    value_coming = HEADER_NONE;
}

// This is called on the first non-pseudo-header. Select the appropriate URI form based on the
// provided pseudo-headers and generate the start line
bool Http2RequestLine::generate_start_line()
{
    uint8_t *start_line_buffer;
    uint32_t bytes_written = 0;

    // Asterisk form - used for OPTIONS requests
    if (path.length() > 0 and path.start()[0] == '*')
    {
        if (method.length() <= 0)
        {
            *session_data->infractions[source_id] += INF_PSEUDO_HEADER_URI_FORM_MISMATCH;
            session_data->events[source_id]->create_event(EVENT_MISFORMATTED_HTTP2);
            return false;
        }
        start_line_length = method.length() + path.length() + http_version_length +
            start_line_extra_chars;
        start_line_buffer = new uint8_t[start_line_length];

        memcpy(start_line_buffer, method.start(), method.length());
        bytes_written += method.length();
        memcpy(start_line_buffer + bytes_written, " ", 1);
        bytes_written += 1;
        memcpy(start_line_buffer + bytes_written, path.start(), path.length());
        bytes_written += path.length();
        memcpy(start_line_buffer + bytes_written, " ", 1);
        bytes_written += 1;
        memcpy(start_line_buffer + bytes_written, http_version_string, http_version_length);
        bytes_written += http_version_length;
    }
    // Authority form - used for CONNECT requests
    else if (method.length() == CONNECT_LENGTH and memcmp(method.start(),
        CONNECT, method.length()) == 0)
    {
        // Must have an authority and not have a scheme or path
        // FIXIT-L May want to be more lenient than RFC on generating start line
        if ( scheme.length() > 0 or path.length() > 0 or authority.length() <= 0)
        {
            *session_data->infractions[source_id] += INF_PSEUDO_HEADER_URI_FORM_MISMATCH;
            session_data->events[source_id]->create_event(EVENT_MISFORMATTED_HTTP2);
            return false;
        }
        start_line_length = method.length() + authority.length() + http_version_length +
            start_line_extra_chars;
        start_line_buffer = new uint8_t[start_line_length];

        memcpy(start_line_buffer, method.start(), method.length());
        bytes_written += method.length();
        memcpy(start_line_buffer + bytes_written, " ", 1);
        bytes_written += 1;
        memcpy(start_line_buffer + bytes_written, authority.start(), authority.length());
        bytes_written += authority.length();
        memcpy(start_line_buffer + bytes_written, " ", 1);
        bytes_written += 1;
        memcpy(start_line_buffer + bytes_written, http_version_string, http_version_length);
        bytes_written += http_version_length;
    }
    // HTTP/2 requests with URIs in absolute or origin form must have a method, scheme, and length
    else if (method.length() > 0 and scheme.length() > 0 and path.length() > 0)
    {
        // If there is an authority, the URI is in absolute form
        if (authority.length() > 0)
        {
            start_line_length = method.length() + scheme.length() + authority.length() +
                path.length() + http_version_length + start_line_extra_chars +
                absolute_form_extra_chars_num;
            start_line_buffer = new uint8_t[start_line_length];

            memcpy(start_line_buffer, method.start(), method.length());
            bytes_written += method.length();
            memcpy(start_line_buffer + bytes_written, " ", 1);
            bytes_written += 1;
            memcpy(start_line_buffer + bytes_written, scheme.start(), scheme.length());
            bytes_written += scheme.length();
            memcpy(start_line_buffer + bytes_written, "://", 3);
            bytes_written += 3;
            memcpy(start_line_buffer + bytes_written, authority.start(), authority.length());
            bytes_written += authority.length();
            memcpy(start_line_buffer + bytes_written, path.start(), path.length());
            bytes_written += path.length();
            memcpy(start_line_buffer + bytes_written, " ", 1);
            bytes_written += 1;
            memcpy(start_line_buffer + bytes_written, http_version_string, http_version_length);
            bytes_written += http_version_length;
        }
        // If there is no authority, the URI is in origin form
        else
        {
            start_line_length = method.length() + path.length() + http_version_length +
                start_line_extra_chars;
            start_line_buffer = new uint8_t[start_line_length];

            memcpy(start_line_buffer, method.start(), method.length());
            bytes_written += method.length();
            memcpy(start_line_buffer + bytes_written, " ", 1);
            bytes_written += 1;
            memcpy(start_line_buffer + bytes_written, path.start(), path.length());
            bytes_written += path.length();
            memcpy(start_line_buffer + bytes_written, " ", 1);
            bytes_written += 1;
            memcpy(start_line_buffer + bytes_written, http_version_string, http_version_length);
            bytes_written += http_version_length;
        }
    }
    else
    {
        // FIXIT-L May want to be more lenient than RFC on generating start line
        *session_data->infractions[source_id] += INF_PSEUDO_HEADER_URI_FORM_MISMATCH;
        session_data->events[source_id]->create_event(EVENT_MISFORMATTED_HTTP2);
        return false;
    }

    memcpy(start_line_buffer + bytes_written, "\r\n", 2);
    bytes_written += 2;
    assert(bytes_written == start_line_length);

    start_line.set(start_line_length, start_line_buffer, true);

    return true;
}
