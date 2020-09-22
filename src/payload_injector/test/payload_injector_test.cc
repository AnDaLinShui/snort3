//--------------------------------------------------------------------------
// Copyright (C) 2020-2020 Cisco and/or its affiliates. All rights reserved.
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

// payload_injector_test.cc author Maya Dagon <mdagon@cisco.com>
// unit test main

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "payload_injector/payload_injector_module.h"

#include "detection/detection_engine.h"
#include "flow/flow.h"
#include "packet_io/active.h"
#include "protocols/packet.h"
#include "utils/util.h"
#include "service_inspectors/http_inspect/http_enum.h"
#include "service_inspectors/http2_inspect/http2_flow_data.h"

#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>

using namespace snort;
using namespace HttpCommon;

//--------------------------------------------------------------------------
// mocks
//--------------------------------------------------------------------------
namespace snort
{
uint32_t Active::send_data(snort::Packet*, EncodeFlags, unsigned char const*, unsigned int)
{
    return 1;
}

void Active::block_session(snort::Packet*, bool) { }
void DetectionEngine::disable_all(snort::Packet*) { }
Flow::Flow()
{
    gadget = nullptr;
    flow_state = Flow::FlowState::SETUP;
}
Flow::~Flow() { }
Packet::Packet(bool) { packet_flags = 0; flow = nullptr; }
Packet::~Packet() { }
int DetectionEngine::queue_event(unsigned int, unsigned int, snort::Actions::Type) { return 0; }
FlowData::~FlowData() { }
FlowData::FlowData(unsigned int, snort::Inspector*) { }

// Inspector mocks, used by MockInspector class
InspectApi mock_api;
Inspector::Inspector()
{
    set_api(&mock_api);
}

Inspector::~Inspector() = default;
bool Inspector::likes(Packet*) { return true; }
bool Inspector::get_buf(const char*, Packet*, InspectionBuffer&) { return true; }
class StreamSplitter* Inspector::get_splitter(bool) { return nullptr; }
}

void show_stats(PegCount*, const PegInfo*, unsigned, const char*) { }
void show_stats(PegCount*, const PegInfo*, const IndexVec&, const char*, FILE*) { }

// MockInspector class

class MockInspector : public snort::Inspector
{
public:

    MockInspector() { }
    ~MockInspector() override { }
    void eval(snort::Packet*) override { }
    bool configure(snort::SnortConfig*) override { return true; }
};

// Mocks for PayloadInjectorModule::get_http2_payload

static InjectionReturnStatus translation_status = INJECTION_SUCCESS;
InjectionReturnStatus PayloadInjectorModule::get_http2_payload(InjectionControl,
    uint8_t*& http2_payload, uint32_t& payload_len)
{
    if (translation_status == INJECTION_SUCCESS)
    {
        http2_payload = (uint8_t*)snort_alloc(1);
        payload_len = 1;
    }

    return translation_status;
}

// Mocks for snort::Flow::get_flow_data

unsigned Http2FlowData::inspector_id = 0;
Http2Stream::~Http2Stream() { }
HpackDynamicTable::~HpackDynamicTable() { }
Http2FlowData::Http2FlowData(snort::Flow*) :
    FlowData(inspector_id),
    flow(nullptr),
    hi(nullptr),
    hpack_decoder
    {
        Http2HpackDecoder(this, SRC_CLIENT, events[SRC_CLIENT], infractions[SRC_CLIENT]),
	Http2HpackDecoder(this, SRC_SERVER, events[SRC_SERVER], infractions[SRC_SERVER])
    }
    { }
Http2FlowData::~Http2FlowData() { }
Http2FlowData http2_flow_data(nullptr);
FlowData* snort::Flow::get_flow_data(unsigned int) const { return &http2_flow_data; }

TEST_GROUP(payload_injector_test)
{
    PayloadInjectorModule mod;
    InjectionControl control;
    PayloadInjectorCounts* counts = (PayloadInjectorCounts*)mod.get_counts();
    Flow flow;
    Active active;

    void setup() override
    {
        counts->http_injects = 0;
        counts->http2_injects = 0;
        counts->http2_translate_err = 0;
        counts->http2_mid_frame = 0;
        control.http_page = (const uint8_t*)"test";
        control.http_page_len = 4;
        flow.set_state(Flow::FlowState::INSPECT);
        translation_status = INJECTION_SUCCESS;
        http2_flow_data.set_continuation_expected(SRC_SERVER, false);
        http2_flow_data.set_reading_frame(SRC_SERVER, false);
    }
};

TEST(payload_injector_test, not_configured_stream_not_established)
{
    mod.set_configured(false);
    Packet p(false);
    p.flow = &flow;
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http_injects == 0);
    CHECK(status == ERR_INJECTOR_NOT_CONFIGURED);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
    const char* err_string = mod.get_err_string(status);
    CHECK(strcmp(err_string, "Payload injector is not configured") == 0);
}

TEST(payload_injector_test, not_configured_stream_established)
{
    mod.set_configured(false);
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    p.flow = &flow;
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http_injects == 0);
    CHECK(status == ERR_INJECTOR_NOT_CONFIGURED);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
}

TEST(payload_injector_test, configured_stream_not_established)
{
    mod.set_configured(true);
    Packet p(false);
    p.flow = &flow;
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http_injects == 0);
    CHECK(status == ERR_STREAM_NOT_ESTABLISHED);
    const char* err_string = mod.get_err_string(status);
    CHECK(strcmp(err_string, "TCP stream not established") == 0);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
}

TEST(payload_injector_test, configured_stream_established)
{
    mod.set_configured(true);
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    mock_api.base.name = "http_inspect";
    flow.gadget = new MockInspector();
    p.flow = &flow;
    p.active = &active;
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http_injects == 1);
    CHECK(status == INJECTION_SUCCESS);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
    delete flow.gadget;
}

TEST(payload_injector_test, http2_stream0)
{
    mod.set_configured(true);
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    mock_api.base.name = "http2_inspect";
    flow.gadget = new MockInspector();
    p.flow = &flow;
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http2_injects == 0);
    CHECK(status == ERR_HTTP2_STREAM_ID_0);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
    const char* err_string = mod.get_err_string(status);
    CHECK(strcmp(err_string, "HTTP/2 - injection to stream 0") == 0);
    delete flow.gadget;
}

TEST(payload_injector_test, http2_even_stream_id)
{
    mod.set_configured(true);
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    mock_api.base.name = "http2_inspect";
    flow.gadget = new MockInspector();
    p.flow = &flow;
    control.stream_id = 2;
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http2_injects == 0);
    CHECK(status == ERR_HTTP2_EVEN_STREAM_ID);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
    const char* err_string = mod.get_err_string(status);
    CHECK(strcmp(err_string, "HTTP/2 - injection to server initiated stream") == 0);
    delete flow.gadget;
}

TEST(payload_injector_test, http2_success)
{
    mod.set_configured(true);
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    mock_api.base.name = "http2_inspect";
    flow.gadget = new MockInspector();
    p.flow = &flow;
    p.active = &active;
    control.stream_id = 1;
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http2_injects == 1);
    CHECK(status == INJECTION_SUCCESS);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
    delete flow.gadget;
}

TEST(payload_injector_test, unidentified_gadget_is_null)
{
    mod.set_configured(true);
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    p.flow = &flow;
    p.active = &active;
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http_injects == 1);
    CHECK(status == INJECTION_SUCCESS);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
}

TEST(payload_injector_test, unidentified_gadget_name)
{
    mod.set_configured(true);
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    mock_api.base.name = "inspector";
    flow.gadget = new MockInspector();
    p.flow = &flow;
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(status == ERR_UNIDENTIFIED_PROTOCOL);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
    delete flow.gadget;
}

TEST(payload_injector_test, http2_mid_frame)
{
    mod.set_configured(true);
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    mock_api.base.name = "http2_inspect";
    flow.gadget = new MockInspector();
    p.flow = &flow;
    control.stream_id = 1;
    http2_flow_data.set_reading_frame(SRC_SERVER, true);
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http2_mid_frame == 1);
    CHECK(status == ERR_HTTP2_MID_FRAME);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
    const char* err_string = mod.get_err_string(status);
    CHECK(strcmp(err_string, "HTTP/2 - attempt to inject mid frame. Currently not supported.")
        == 0);
    delete flow.gadget;
}

TEST(payload_injector_test, http2_continuation_expected)
{
    mod.set_configured(true);
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    mock_api.base.name = "http2_inspect";
    flow.gadget = new MockInspector();
    p.flow = &flow;
    control.stream_id = 1;
    http2_flow_data.set_continuation_expected(SRC_SERVER, true);
    InjectionReturnStatus status = mod.inject_http_payload(&p, control);
    CHECK(counts->http2_mid_frame == 1);
    CHECK(status == ERR_HTTP2_MID_FRAME);
    CHECK(flow.flow_state == Flow::FlowState::BLOCK);
    delete flow.gadget;
}

TEST_GROUP(payload_injector_translate_err_test)
{
    PayloadInjectorModule mod;
    InjectionControl control;
    PayloadInjectorCounts* counts = (PayloadInjectorCounts*)mod.get_counts();
    Flow flow;
    InjectionReturnStatus status = INJECTION_SUCCESS;

    void setup() override
    {
        counts->http_injects = 0;
        counts->http2_injects = 0;
        counts->http2_translate_err = 0;
        counts->http2_mid_frame = 0;
        control.http_page = (const uint8_t*)"test";
        control.http_page_len = 4;
        flow.set_state(Flow::FlowState::INSPECT);
        http2_flow_data.set_continuation_expected(SRC_SERVER, false);
        http2_flow_data.set_reading_frame(SRC_SERVER, false);
        mod.set_configured(true);
        mock_api.base.name = "http2_inspect";
        flow.gadget = new MockInspector();
        control.stream_id = 1;
    }

    void teardown() override
    {
        CHECK(counts->http2_translate_err == 1);
        CHECK(status == translation_status);
        CHECK(flow.flow_state == Flow::FlowState::BLOCK);
        delete flow.gadget;
    }
};

TEST(payload_injector_translate_err_test, http2_page_translation_err)
{
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    p.flow = &flow;
    translation_status = ERR_PAGE_TRANSLATION;
    status = mod.inject_http_payload(&p, control);
    const char* err_string = mod.get_err_string(status);
    CHECK(strcmp(err_string, "Error in translating HTTP block page to HTTP/2. "
        "Unsupported or bad format.") == 0);
}

TEST(payload_injector_translate_err_test, http2_hdrs_size)
{
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    p.flow = &flow;
    translation_status = ERR_TRANSLATED_HDRS_SIZE;
    status = mod.inject_http_payload(&p, control);
    const char* err_string = mod.get_err_string(status);
    CHECK(strcmp(err_string,
        "HTTP/2 translated header size is bigger than expected. Update max size.") == 0);
}

TEST(payload_injector_translate_err_test, http2_body_size)
{
    Packet p(false);
    p.packet_flags = PKT_STREAM_EST;
    p.flow = &flow;
    translation_status = ERR_HTTP2_BODY_SIZE;
    status = mod.inject_http_payload(&p, control);
    const char* err_string = mod.get_err_string(status);
    CHECK(strcmp(err_string, "HTTP/2 body is > 16k. Currently not supported.") == 0);
}

int main(int argc, char** argv)
{
    return CommandLineTestRunner::RunAllTests(argc, argv);
}

