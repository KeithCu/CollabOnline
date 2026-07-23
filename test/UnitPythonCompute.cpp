/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Copyright the Collabora Online contributors.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <config.h>

#include <WopiTestServer.hpp>
#include <common/JsonUtil.hpp>
#include <common/Util.hpp>
#include <lokassert.hpp>

#include <Poco/Net/HTTPRequest.h>
#include <Poco/StreamCopier.h>
#include <Poco/URI.h>
#include <Poco/Util/Application.h>
#include <Poco/Util/LayeredConfiguration.h>

#include <sstream>
#include <string>
#include <string_view>

class UnitPythonCompute final : public WopiTestServer
{
    enum class Phase
    {
        Load,
        WaitLoad,
        WaitResult,
        Done
    };

    const bool _enabled;
    Phase _phase = Phase::Load;
    std::size_t _postCount = 0;

public:
    explicit UnitPythonCompute(bool enabled)
        : WopiTestServer(enabled ? "UnitPythonComputeEnabled" : "UnitPythonComputeDisabled",
                         "empty.ods")
        , _enabled(enabled)
    {
    }

    void configure(Poco::Util::LayeredConfiguration& config) override
    {
        WopiTestServer::configure(config);
        config.setBool("security.python_compute.enable", _enabled);
        config.setInt("security.python_compute.timeout_secs", 10);
    }

    bool handleHttpRequest(const Poco::Net::HTTPRequest& request, std::istream& message,
                           const std::shared_ptr<StreamSocket>& socket) override
    {
        const Poco::URI uri(request.getURI());
        if (uri.getPath() != "/v1/execute")
            return WopiTestServer::handleHttpRequest(request, message, socket);

        LOK_ASSERT(_enabled);
        LOK_ASSERT_EQUAL_STR("POST", request.getMethod());
        ++_postCount;

        std::ostringstream bodyStream;
        Poco::StreamCopier::copyStream(message, bodyStream);
        Poco::JSON::Object::Ptr requestObject;
        LOK_ASSERT(JsonUtil::parseJSON(bodyStream.str(), requestObject));
        LOK_ASSERT(requestObject);

        std::string requestId;
        std::string code;
        LOK_ASSERT(JsonUtil::findJSONValue(requestObject, "id", requestId));
        LOK_ASSERT(JsonUtil::findJSONValue(requestObject, "code", code));
        LOK_ASSERT_EQUAL_STR("wire-1", requestId);
        LOK_ASSERT_EQUAL_STR("result=1", code);

        Poco::JSON::Object::Ptr result = new Poco::JSON::Object();
        result->set("id", requestId);
        result->set("status", "ok");
        result->set("result", 42);
        std::ostringstream resultStream;
        result->stringify(resultStream);

        http::Response response(http::StatusCode::OK);
        response.setBody(resultStream.str(), "application/json; charset=utf-8");
        socket->sendAndShutdown(response);
        return true;
    }

    bool onDocumentLoaded(const std::string& message) override
    {
        TST_LOG("Python compute document loaded: [" << message << ']');
        LOK_ASSERT(_phase == Phase::WaitLoad);
        _phase = Phase::WaitResult;
        WSD_CMD(R"(pythonexecute {"id":"wire-1","code":"result=1"})");
        return true;
    }

    bool onFilterSendWebSocketMessage(std::string_view data, WSOpCode, bool,
                                      int&) override
    {
        constexpr std::string_view prefix = "pythoncomputeresult:";
        if (!data.starts_with(prefix))
            return false;

        LOK_ASSERT(_phase == Phase::WaitResult);
        Poco::JSON::Object::Ptr result;
        LOK_ASSERT(JsonUtil::parseJSON(Util::trimmed(data.substr(prefix.size())), result));
        LOK_ASSERT(result);

        std::string requestId;
        std::string status;
        LOK_ASSERT(JsonUtil::findJSONValue(result, "id", requestId));
        LOK_ASSERT(JsonUtil::findJSONValue(result, "status", status));
        LOK_ASSERT_EQUAL_STR("wire-1", requestId);

        if (_enabled)
        {
            int value = 0;
            LOK_ASSERT_EQUAL_STR("ok", status);
            LOK_ASSERT(JsonUtil::findJSONValue(result, "result", value));
            LOK_ASSERT_EQUAL(42, value);
            LOK_ASSERT_EQUAL(static_cast<std::size_t>(1), _postCount);
        }
        else
        {
            std::string error;
            LOK_ASSERT_EQUAL_STR("error", status);
            LOK_ASSERT(JsonUtil::findJSONValue(result, "error", error));
            LOK_ASSERT_EQUAL_STR("Python compute is disabled", error);
            LOK_ASSERT_EQUAL(static_cast<std::size_t>(0), _postCount);
        }

        _phase = Phase::Done;
        passTest(_enabled ? "Python compute POST/result wire passed"
                          : "Python compute disabled path avoided POST");
        return true;
    }

    void invokeWSDTest() override
    {
        if (_phase != Phase::Load)
            return;

        // ClientPortNumber is assigned after configure(), so set the loopback
        // endpoint only once the WSD listener has its final port.
        auto& config = Poco::Util::Application::instance().config();
        config.setBool("security.python_compute.enable", _enabled);
        config.setString("security.python_compute.url",
                         helpers::getTestServerURI() + "/v1/execute");
        _phase = Phase::WaitLoad;
        initWebsocket("/wopi/files/0?access_token=anything");
        WSD_CMD("load url=" + getWopiSrc());
    }
};

UnitBase** unit_create_wsd_multi(void)
{
    return new UnitBase*[3]{ new UnitPythonCompute(true), new UnitPythonCompute(false), nullptr };
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
