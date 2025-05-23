#include <memory>

#include "envoy/extensions/filters/http/ip_tagging/v3/ip_tagging.pb.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/network/address_impl.h"
#include "source/common/network/utility.h"
#include "source/extensions/filters/http/ip_tagging/ip_tagging_filter.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Return;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace IpTagging {
namespace {

class IpTaggingFilterTest : public testing::Test {
public:
  IpTaggingFilterTest() {
    ON_CALL(runtime_.snapshot_, featureEnabled("ip_tagging.http_filter_enabled", 100))
        .WillByDefault(Return(true));
  }

  const std::string internal_request_yaml = R"EOF(
request_type: internal
ip_tags:
  - ip_tag_name: internal_request
    ip_list:
      - {address_prefix: 1.2.3.5, prefix_len: 32}

)EOF";

  void initializeFilter(const std::string& yaml,
                        absl::optional<std::string> expected_error = absl::nullopt) {
    envoy::extensions::filters::http::ip_tagging::v3::IPTagging config;
    TestUtility::loadFromYaml(yaml, config);
    auto config_or =
        IpTaggingFilterConfig::create(config, "prefix.", *stats_.rootScope(), runtime_);
    if (expected_error.has_value()) {
      EXPECT_FALSE(config_or.ok());
      EXPECT_EQ(expected_error.value(), absl::StrCat(config_or.status()));
      return;
    }
    EXPECT_TRUE(config_or.ok());
    config_ = std::move(config_or.value());
    filter_ = std::make_unique<IpTaggingFilter>(config_);
    filter_->setDecoderFilterCallbacks(filter_callbacks_);
  }

  ~IpTaggingFilterTest() override {
    if (filter_ != nullptr) {
      filter_->onDestroy();
    }
  }

  NiceMock<Stats::MockStore> stats_;
  IpTaggingFilterConfigSharedPtr config_;
  std::unique_ptr<IpTaggingFilter> filter_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> filter_callbacks_;
  Buffer::OwnedImpl data_;
  NiceMock<Runtime::MockLoader> runtime_;
};

TEST_F(IpTaggingFilterTest, InternalRequest) {
  initializeFilter(internal_request_yaml);
  EXPECT_EQ(FilterRequestType::INTERNAL, config_->requestType());
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"}};

  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.5");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_CALL(stats_, counter("prefix.ip_tagging.internal_request.hit"));
  EXPECT_CALL(stats_, counter("prefix.ip_tagging.total"));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("internal_request", request_headers.get_(Http::Headers::get().EnvoyIpTags));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));

  // Check external requests don't get a tag.
  request_headers = Http::TestRequestHeaderMapImpl{};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_FALSE(request_headers.has(Http::Headers::get().EnvoyIpTags));
}

TEST_F(IpTaggingFilterTest, ExternalRequest) {
  const std::string external_request_yaml = R"EOF(
request_type: external
ip_tags:
  - ip_tag_name: external_request
    ip_list:
      - {address_prefix: 1.2.3.4, prefix_len: 32}
)EOF";
  initializeFilter(external_request_yaml);
  EXPECT_EQ(FilterRequestType::EXTERNAL, config_->requestType());
  Http::TestRequestHeaderMapImpl request_headers;

  EXPECT_CALL(stats_, counter("prefix.ip_tagging.total"));
  EXPECT_CALL(stats_, counter("prefix.ip_tagging.external_request.hit"));

  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.4");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("external_request", request_headers.get_(Http::Headers::get().EnvoyIpTags));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));

  // Check internal requests don't get a tag.
  request_headers = {{"x-envoy-internal", "true"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_FALSE(request_headers.has(Http::Headers::get().EnvoyIpTags));
}

TEST_F(IpTaggingFilterTest, BothRequest) {
  const std::string both_request_yaml = R"EOF(
request_type: both
ip_tags:
  - ip_tag_name: external_request
    ip_list:
      - {address_prefix: 1.2.3.4, prefix_len: 32}
  - ip_tag_name: internal_request
    ip_list:
      - {address_prefix: 1.2.3.5, prefix_len: 32}
)EOF";

  initializeFilter(both_request_yaml);
  EXPECT_EQ(FilterRequestType::BOTH, config_->requestType());
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"}};

  EXPECT_CALL(stats_, counter("prefix.ip_tagging.total")).Times(2);
  EXPECT_CALL(stats_, counter("prefix.ip_tagging.internal_request.hit"));
  EXPECT_CALL(stats_, counter("prefix.ip_tagging.external_request.hit"));

  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.5");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("internal_request", request_headers.get_(Http::Headers::get().EnvoyIpTags));

  request_headers = Http::TestRequestHeaderMapImpl{};
  remote_address = Network::Utility::parseInternetAddressNoThrow("1.2.3.4");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("external_request", request_headers.get_(Http::Headers::get().EnvoyIpTags));
}

TEST_F(IpTaggingFilterTest, NoHits) {
  initializeFilter(internal_request_yaml);
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"}};

  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("10.2.3.5");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_CALL(stats_, counter("prefix.ip_tagging.no_hit"));
  EXPECT_CALL(stats_, counter("prefix.ip_tagging.total"));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_FALSE(request_headers.has(Http::Headers::get().EnvoyIpTags));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, AppendEntry) {
  initializeFilter(internal_request_yaml);
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"},
                                                 {"x-envoy-ip-tags", "test"}};

  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.5");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("test,internal_request", request_headers.get_(Http::Headers::get().EnvoyIpTags));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, ReplaceAlternateHeaderWhenActionIsDefaulted) {
  const std::string internal_request_yaml = R"EOF(
request_type: internal
ip_tag_header:
  header: x-envoy-optional-header
ip_tags:
  - ip_tag_name: internal_request_with_optional_header
    ip_list:
      - {address_prefix: 1.2.3.4, prefix_len: 32}
)EOF";

  initializeFilter(internal_request_yaml);

  Http::TestRequestHeaderMapImpl request_headers{
      {"x-envoy-internal", "true"},
      {"x-envoy-optional-header", "foo"}, // foo will be removed
      {"x-envoy-optional-header", "bar"}, // bar will be removed
      {"x-envoy-optional-header", "baz"}, // baz will be removed
  };
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.4");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("internal_request_with_optional_header",
            request_headers.get_("x-envoy-optional-header"));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, ReplaceAlternateHeader) {
  const std::string internal_request_yaml = R"EOF(
request_type: internal
ip_tag_header:
  header: x-envoy-optional-header
  action: SANITIZE
ip_tags:
  - ip_tag_name: internal_request_with_optional_header
    ip_list:
      - {address_prefix: 1.2.3.4, prefix_len: 32}
)EOF";

  initializeFilter(internal_request_yaml);

  Http::TestRequestHeaderMapImpl request_headers{
      {"x-envoy-internal", "true"}, {"x-envoy-optional-header", "foo"}}; // foo will be removed
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.4");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("internal_request_with_optional_header",
            request_headers.get_("x-envoy-optional-header"));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, ClearAlternateHeaderWhenUnmatchedAndSanitized) {
  const std::string internal_request_yaml = R"EOF(
request_type: internal
ip_tag_header:
  header: x-envoy-optional-header
  action: SANITIZE
ip_tags:
  - ip_tag_name: internal_request_with_optional_header
    ip_list:
      - {address_prefix: 1.2.3.4, prefix_len: 32}
)EOF";

  initializeFilter(internal_request_yaml);

  Http::TestRequestHeaderMapImpl request_headers{
      {"x-envoy-internal", "true"}, {"x-envoy-optional-header", "foo"}}; // header will be removed
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.5");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_FALSE(request_headers.has("x-envoy-optional-header"));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, AppendForwardAlternateHeader) {
  const std::string internal_request_yaml = R"EOF(
request_type: internal
ip_tag_header:
  header: x-envoy-optional-header
  action: APPEND_IF_EXISTS_OR_ADD
ip_tags:
  - ip_tag_name: internal_request_with_optional_header
    ip_list:
      - {address_prefix: 1.2.3.4, prefix_len: 32}
)EOF";

  initializeFilter(internal_request_yaml);

  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"},
                                                 {"x-envoy-optional-header", "foo"}};
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.4");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("foo,internal_request_with_optional_header",
            request_headers.get_("x-envoy-optional-header"));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, RetainAlternateHeaderWhenUnmatchedAndAppendForwarded) {
  const std::string internal_request_yaml = R"EOF(
request_type: internal
ip_tag_header:
  header: x-envoy-optional-header
  action: APPEND_IF_EXISTS_OR_ADD
ip_tags:
  - ip_tag_name: internal_request_with_optional_header
    ip_list:
      - {address_prefix: 1.2.3.4, prefix_len: 32}
)EOF";

  initializeFilter(internal_request_yaml);

  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"},
                                                 {"x-envoy-optional-header", "foo"}};
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.5");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("foo", request_headers.get_("x-envoy-optional-header"));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, NestedPrefixes) {
  const std::string duplicate_request_yaml = R"EOF(
request_type: both
ip_tags:
  - ip_tag_name: duplicate_request
    ip_list:
      - {address_prefix: 1.2.3.4, prefix_len: 32}
  - ip_tag_name: internal_request
    ip_list:
      - {address_prefix: 1.2.3.4, prefix_len: 32}
)EOF";

  initializeFilter(duplicate_request_yaml);
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"},
                                                 {"x-envoy-ip-tags", "test"}};

  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.4");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_CALL(stats_, counter("prefix.ip_tagging.total"));
  EXPECT_CALL(stats_, counter("prefix.ip_tagging.internal_request.hit"));
  EXPECT_CALL(stats_, counter("prefix.ip_tagging.duplicate_request.hit"));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  // There is no guarantee for the order tags are returned by the LC-Trie.
  const std::string header_tag_data = request_headers.get_(Http::Headers::get().EnvoyIpTags.get());
  EXPECT_NE(std::string::npos, header_tag_data.find("test"));
  EXPECT_NE(std::string::npos, header_tag_data.find("internal_request"));
  EXPECT_NE(std::string::npos, header_tag_data.find("duplicate_request"));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, Ipv6Address) {
  const std::string ipv6_addresses_yaml = R"EOF(
ip_tags:
  - ip_tag_name: ipv6_request
    ip_list:
      - {address_prefix: 2001:abcd:ef01:2345:6789:abcd:ef01:234, prefix_len: 64}
)EOF";
  initializeFilter(ipv6_addresses_yaml);
  Http::TestRequestHeaderMapImpl request_headers;

  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("2001:abcd:ef01:2345::1");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("ipv6_request", request_headers.get_(Http::Headers::get().EnvoyIpTags));

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, RuntimeDisabled) {
  initializeFilter(internal_request_yaml);
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"}};

  EXPECT_CALL(runtime_.snapshot_, featureEnabled("ip_tagging.http_filter_enabled", 100))
      .WillOnce(Return(false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_FALSE(request_headers.has(Http::Headers::get().EnvoyIpTags));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers));
}

TEST_F(IpTaggingFilterTest, ClearRouteCache) {
  initializeFilter(internal_request_yaml);
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"}};

  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddressNoThrow("1.2.3.5");
  filter_callbacks_.stream_info_.downstream_connection_info_provider_->setRemoteAddress(
      remote_address);

  EXPECT_CALL(filter_callbacks_.downstream_callbacks_, clearRouteCache());
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ("internal_request", request_headers.get_(Http::Headers::get().EnvoyIpTags));

  // no tags, no call
  EXPECT_CALL(filter_callbacks_.downstream_callbacks_, clearRouteCache()).Times(0);
  request_headers = Http::TestRequestHeaderMapImpl{};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_FALSE(request_headers.has(Http::Headers::get().EnvoyIpTags));
}

TEST_F(IpTaggingFilterTest, EmptyIpTags) {
  const std::string external_request_yaml = R"EOF(
request_type: external
)EOF";
  initializeFilter(external_request_yaml,
                   "INVALID_ARGUMENT: HTTP IP Tagging Filter requires ip_tags to be specified.");
}

TEST_F(IpTaggingFilterTest, InvalidCidr) {
  const std::string external_request_yaml = R"EOF(
request_type: external
ip_tags:
  - ip_tag_name: fooooooo
    ip_list:
      - {address_prefix: 12345.12345.12345.12345, prefix_len: 999999}
)EOF";
  initializeFilter(
      external_request_yaml,
      "INVALID_ARGUMENT: invalid ip/mask combo '12345.12345.12345.12345/999999' (format is "
      "<ip>/<# mask bits>)");
}

} // namespace
} // namespace IpTagging
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
