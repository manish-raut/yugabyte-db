// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.controllers;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.ApiUtils;
import com.yugabyte.yw.common.CloudQueryHelper;
import com.yugabyte.yw.common.FakeApiHelper;
import com.yugabyte.yw.common.CallHomeManager.CollectionLevel;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import com.yugabyte.yw.models.Customer;

import com.yugabyte.yw.models.AvailabilityZone;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import com.yugabyte.yw.models.Universe;
import org.junit.Before;
import org.junit.Test;
import org.mockito.ArgumentCaptor;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.libs.Json;
import play.mvc.Http;
import play.mvc.Result;
import play.test.Helpers;
import play.test.WithApplication;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import static com.yugabyte.yw.common.AssertHelper.assertBadRequest;
import static com.yugabyte.yw.common.AssertHelper.assertValue;
import static com.yugabyte.yw.common.ModelFactory.createUniverse;
import static org.hamcrest.CoreMatchers.*;
import static org.hamcrest.core.StringContains.containsString;
import static org.mockito.Matchers.anyList;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Mockito.*;
import static play.inject.Bindings.bind;
import static play.test.Helpers.*;
import static org.junit.Assert.*;
import static play.mvc.Http.Status.OK;
import static play.mvc.Http.Status.FORBIDDEN;
import static play.test.Helpers.fakeRequest;

public class CustomerControllerTest extends WithApplication {
  MetricQueryHelper mockMetricQueryHelper;
  CloudQueryHelper mockCloudQueryHelper;
  String baseRoute = "/api/customers/";

  @Override
  protected Application provideApplication() {
    mockMetricQueryHelper = mock(MetricQueryHelper.class);
    Commissioner mockCommissioner = mock(Commissioner.class);
    mockCloudQueryHelper = mock(CloudQueryHelper.class);

    return new GuiceApplicationBuilder()
      .configure((Map) Helpers.inMemoryDatabase())
      .overrides(bind(MetricQueryHelper.class).toInstance(mockMetricQueryHelper))
      .overrides(bind(Commissioner.class).toInstance(mockCommissioner))
      .overrides(bind(CloudQueryHelper.class).toInstance(mockCloudQueryHelper))
      .build();
  }

  private Customer customer;

  @Before
  public void setUp() {
    customer = ModelFactory.testCustomer();
  }

  @Test
  public void testCustomerGETWithValidUUID() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    Result result = route(fakeRequest("GET", baseRoute + customer.uuid).cookie(validCookie));
    assertEquals(OK, result.status());
    JsonNode json = Json.parse(contentAsString(result));

    assertThat(json.get("uuid").asText(), allOf(notNullValue(), equalTo(customer.uuid.toString())));
    assertEquals(json.get("name").asText(), customer.name);
  }

  @Test
  public void testCustomerGETWithInvalidUUID() {
    String authToken = customer.createAuthToken();
    UUID invalidUUID = UUID.randomUUID();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    Result result = route(fakeRequest("GET", baseRoute + invalidUUID).cookie(validCookie));
    assertEquals(FORBIDDEN, result.status());

    String resultString = contentAsString(result);
    assertThat(resultString, allOf(notNullValue(),
        equalTo("Unable To Authenticate Customer")));
  }

  @Test
  public void testCustomerPUTWithValidParams() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    ObjectNode params = Json.newObject();
    params.put("code", "tc");
    params.put("email", "foo@bar.com");
    params.put("name", "Test Customer");
    params.put("password", "new-password");
    params.put("confirmPassword", "new-password");
    ObjectNode alertingData = Json.newObject();
    alertingData.put("sendAlertsToYb", true);
    params.put("alertingData", alertingData);
    params.put("callhomeLevel", "LOW");
    Result result = route(fakeRequest("PUT", baseRoute + customer.uuid).cookie(validCookie).bodyJson(params));
    assertEquals(OK, result.status());
    CustomerConfig callhomeConfig = CustomerConfig.getCallhomeConfig(customer.uuid);
    CollectionLevel callhomeLevel = CustomerConfig.getCallhomeLevel(customer.uuid);
    assertEquals(CollectionLevel.LOW, callhomeLevel);
    JsonNode json = Json.parse(contentAsString(result));
    assertThat(json.get("uuid").asText(), is(equalTo(customer.uuid.toString())));
    assertThat(json.get("name").asText(), is(equalTo("Test Customer")));
  }

  @Test
  public void testCustomerPUTWithDefaultCallhome() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    ObjectNode params = Json.newObject();
    params.put("code", "tc");
    params.put("email", "foo@bar.com");
    params.put("name", "Test Customer");
    params.put("password", "new-password");
    params.put("confirmPassword", "new-password");
    Result result = route(fakeRequest("PUT", baseRoute + customer.uuid).cookie(validCookie).bodyJson(params));
    assertEquals(OK, result.status());
    CustomerConfig callhomeConfig = CustomerConfig.getCallhomeConfig(customer.uuid);
    CollectionLevel callhomeLevel = CustomerConfig.getCallhomeLevel(customer.uuid);
    assertEquals(CollectionLevel.MEDIUM, callhomeLevel);
  }

  @Test
  public void testCustomerPUTWithValidFeatures() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    ObjectNode params = Json.newObject();
    params.put("code", "tc");
    params.put("email", "foo@bar.com");
    params.put("name", "Test Customer");
    JsonNode features = Json.parse("{\"foo\": \"bar\"}");
    params.set("features", features);

    Result result = route(fakeRequest("PUT", baseRoute + customer.uuid).cookie(validCookie).bodyJson(params));
    assertEquals(OK, result.status());
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(features, json.get("features"));
  }

  @Test
  public void testCustomerPUTWithInvalidFeatures() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    ObjectNode params = Json.newObject();
    params.put("code", "tc");
    params.put("email", "foo@bar.com");
    params.put("name", "Test Customer");
    params.put("features", "foo");

    Result result = route(fakeRequest("PUT", baseRoute + customer.uuid).cookie(validCookie).bodyJson(params));
    assertBadRequest(result, "{\"features\":[\"Invalid value\"]}");
  }

  @Test
  public void testFeatureUpsert() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    JsonNode inputFeatures = Json.parse("{\"features\": {\"foo\": \"bar\", \"key\": \"old\"}}");
    JsonNode expectedFeatures = Json.parse("{\"foo\": \"bar\", \"key\": \"old\"}");

    Result result = route(fakeRequest("PUT", baseRoute + customer.uuid + "/features").cookie(validCookie).bodyJson(inputFeatures));
    assertEquals(OK, result.status());
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(expectedFeatures, json);

    inputFeatures = Json.parse("{\"features\": {\"key\": \"new\"}}");
    expectedFeatures = Json.parse("{\"foo\": \"bar\", \"key\": \"new\"}");
    result = route(fakeRequest("PUT", baseRoute + customer.uuid + "/features").cookie(validCookie).bodyJson(inputFeatures));
    assertEquals(OK, result.status());
    json = Json.parse(contentAsString(result));
    assertEquals(expectedFeatures, json);
  }

  @Test
  public void testCustomerPUTWithInvalidParams() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    ObjectNode params = Json.newObject();
    params.put("password", "new-password");

    Result result = route(fakeRequest("PUT", baseRoute + customer.uuid).cookie(validCookie).bodyJson(params));
    assertEquals(BAD_REQUEST, result.status());
    JsonNode json = Json.parse(contentAsString(result));
    assertThat(contentAsString(result), is(containsString("\"name\":[\"This field is required\"]")));
    assertThat(contentAsString(result), is(containsString("\"email\":[\"This field is required\"]")));
  }

  @Test
  public void testCustomerPUTWithInvalidUUID() {
    String authToken = customer.createAuthToken();
    UUID invalidUUID = UUID.randomUUID();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    Result result = route(fakeRequest("PUT", baseRoute + invalidUUID).cookie(validCookie));
    assertEquals(FORBIDDEN, result.status());

    String resultString = contentAsString(result);
    assertThat(resultString, allOf(notNullValue(),
        equalTo("Unable To Authenticate Customer")));
  }

  @Test
  public void testCustomerDELETEWithValidUUID() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    Result result = route(fakeRequest("DELETE", baseRoute + customer.uuid).cookie(validCookie));
    assertEquals(OK, result.status());
    JsonNode json = Json.parse(contentAsString(result));
    assertTrue(json.get("success").asBoolean());
  }

  @Test
  public void testCustomerDELETEWithInvalidUUID() {
    UUID invalidUUID = UUID.randomUUID();

    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    Result result = route(fakeRequest("DELETE", baseRoute + invalidUUID).cookie(validCookie));
    assertEquals(FORBIDDEN, result.status());

    String resultString = contentAsString(result);
    assertThat(resultString, allOf(notNullValue(),
        equalTo("Unable To Authenticate Customer")));
  }

  @Test
  public void testCustomerMetricsWithInvalidParams() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    ObjectNode params = Json.newObject();

    Result result = route(fakeRequest("POST", baseRoute + customer.uuid + "/metrics").cookie(validCookie).bodyJson(params));

    assertEquals(BAD_REQUEST, result.status());
    assertThat(contentAsString(result), is(containsString("\"start\":[\"This field is required\"]")));
    assertThat(contentAsString(result), is(containsString("\"metrics\":[\"This field is required\"]")));
  }

  @Test
  public void testCustomerMetricsWithValidMetricsParams() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("metrics")));
    params.put("start", "1479281737000");

    ObjectNode response = Json.newObject();
    response.put("foo", "bar");

    when(mockMetricQueryHelper.query(anyList(), anyMap())).thenReturn(response);
    Result result = route(fakeRequest("POST", baseRoute + customer.uuid + "/metrics").cookie(validCookie).bodyJson(params));
    assertEquals(OK, result.status());
    assertThat(contentAsString(result), allOf(notNullValue(), containsString("{\"foo\":\"bar\"}")));
  }

  @Test
  public void testCustomerMetricsForContainerMetricsMultiAZ() {
    String authToken = customer.createAuthToken();
    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("container_metrics")));
    params.put("start", "1479281737000");
    params.put("nodePrefix", "demo");
    Universe u1 = createUniverse("demo", customer.getCustomerId());
    Provider provider = Provider.get(UUID.fromString(u1.getUniverseDetails()
                                         .getPrimaryCluster()
                                         .userIntent.provider));
    Region r = Region.create(provider, "region-1", "PlacementRegion-1", "default-image");
    Region r1 = Region.create(provider, "region-2", "PlacementRegion-2", "default-image");

    ObjectNode response = Json.newObject();
    response.put("foo", "bar");
    ArgumentCaptor<ArrayList> metricKeys = ArgumentCaptor.forClass(ArrayList.class);
    ArgumentCaptor<Map> queryParams = ArgumentCaptor.forClass(Map.class);
    when(mockMetricQueryHelper.query(anyList(), anyMap())).thenReturn(response);
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("POST",
        baseRoute + customer.uuid + "/metrics", authToken, params);
    verify(mockMetricQueryHelper).query((List<String>) metricKeys.capture(),
        (Map<String, String>) queryParams.capture());
    assertThat(queryParams.getValue(), is(notNullValue()));
    JsonNode filters = Json.parse(queryParams.getValue().get("filters").toString());
    assertValue(filters, "namespace", "demo-(.*)");
    assertEquals(OK, result.status());
    assertThat(contentAsString(result), allOf(notNullValue(), containsString("{\"foo\":\"bar\"}")));
  }

  @Test
  public void testCustomerMetricsForContainerMetricsSingleAZ() {
    String authToken = customer.createAuthToken();
    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("container_metrics")));
    params.put("start", "1479281737000");
    params.put("nodePrefix", "demo");
    Universe u1 = createUniverse("demo", customer.getCustomerId());
    Provider provider = Provider.get(UUID.fromString(u1.getUniverseDetails()
                                         .getPrimaryCluster()
                                         .userIntent.provider));
    Region r = Region.create(provider, "region-1", "PlacementRegion-1", "default-image");
    AvailabilityZone.create(r, "az-1", "PlacementAZ-1", "subnet-1");

    ObjectNode response = Json.newObject();
    response.put("foo", "bar");
    ArgumentCaptor<ArrayList> metricKeys = ArgumentCaptor.forClass(ArrayList.class);
    ArgumentCaptor<Map> queryParams = ArgumentCaptor.forClass(Map.class);
    when(mockMetricQueryHelper.query(anyList(), anyMap())).thenReturn(response);
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("POST",
        baseRoute + customer.uuid + "/metrics", authToken, params);
    verify(mockMetricQueryHelper).query((List<String>) metricKeys.capture(),
        (Map<String, String>) queryParams.capture());
    assertThat(queryParams.getValue(), is(notNullValue()));
    JsonNode filters = Json.parse(queryParams.getValue().get("filters").toString());
    assertValue(filters, "namespace", "demo");
    assertEquals(OK, result.status());
    assertThat(contentAsString(result), allOf(notNullValue(), containsString("{\"foo\":\"bar\"}")));
  }

  @Test
  public void testCustomerMetricsForContainerMetricsWithNodeName() {
    String authToken = customer.createAuthToken();
    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("container_metrics")));
    params.put("start", "1479281737000");
    params.put("nodePrefix", "demo");
    params.put("nodeName", "demo-n1");
    ObjectNode response = Json.newObject();
    response.put("foo", "bar");
    ArgumentCaptor<ArrayList> metricKeys = ArgumentCaptor.forClass(ArrayList.class);
    ArgumentCaptor<Map> queryParams = ArgumentCaptor.forClass(Map.class);
    when(mockMetricQueryHelper.query(anyList(), anyMap())).thenReturn(response);
    Result result = FakeApiHelper.doRequestWithAuthTokenAndBody("POST",
        baseRoute + customer.uuid + "/metrics", authToken, params);
    verify(mockMetricQueryHelper).query((List<String>) metricKeys.capture(),
        (Map<String, String>) queryParams.capture());
    assertThat(queryParams.getValue(), is(notNullValue()));
    JsonNode filters = Json.parse(queryParams.getValue().get("filters").toString());
    assertValue(filters, "namespace", "demo");
    assertValue(filters, "pod_name", "demo-n1");
    assertEquals(OK, result.status());
    assertThat(contentAsString(result), allOf(notNullValue(), containsString("{\"foo\":\"bar\"}")));
  }

  @Test
  public void testCustomerMetricsWithInValidMetricsParam() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("metric1")));
    params.put("start", "1479281737");

    ObjectNode response = Json.newObject();
    response.put("error", "something went wrong");

    when(mockMetricQueryHelper.query(anyList(), anyMap())).thenReturn(response);
    Result result = route(fakeRequest("POST", baseRoute + customer.uuid + "/metrics").cookie(validCookie).bodyJson(params));
    assertEquals(BAD_REQUEST, result.status());
    assertThat(contentAsString(result), allOf(notNullValue(), containsString("{\"error\":\"something went wrong\"}")));
  }

  @Test
  public void testCustomerMetricsWithValidTableNameParams() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    Universe u1 = createUniverse("Foo-1", customer.getCustomerId());
    u1 = Universe.saveDetails(u1.universeUUID, ApiUtils.mockUniverseUpdater("host-1"));
    Universe u2 = createUniverse("Foo-2", customer.getCustomerId());
    u2 = Universe.saveDetails(u2.universeUUID, ApiUtils.mockUniverseUpdater("host-2"));
    customer.addUniverseUUID(u1.universeUUID);
    customer.addUniverseUUID(u2.universeUUID);
    customer.save();

    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("metric")));
    params.put("start", "1479281737");
    params.put("nodePrefix", "host-1");
    params.put("tableName", "redis");

    ArgumentCaptor<ArrayList> metricKeys = ArgumentCaptor.forClass(ArrayList.class);
    ArgumentCaptor<Map> queryParams = ArgumentCaptor.forClass(Map.class);
    route(fakeRequest("POST", baseRoute + customer.uuid + "/metrics").cookie(validCookie).bodyJson(params));
    verify(mockMetricQueryHelper).query((List<String>) metricKeys.capture(),
      (Map<String, String>) queryParams.capture());
    assertThat(queryParams.getValue(), is(notNullValue()));
    JsonNode filters = Json.parse(queryParams.getValue().get("filters").toString());
    String tableName = filters.get("table_name").asText();
    assertThat(tableName, allOf(notNullValue(), equalTo("redis")));
  }

  @Test
  public void testCustomerMetricsWithoutTableNameParams() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    Universe u1 = createUniverse("Foo-1", customer.getCustomerId());
    u1 = Universe.saveDetails(u1.universeUUID, ApiUtils.mockUniverseUpdater("host-1"));
    Universe u2 = createUniverse("Foo-2", customer.getCustomerId());
    u2 = Universe.saveDetails(u2.universeUUID, ApiUtils.mockUniverseUpdater("host-2"));
    customer.addUniverseUUID(u1.universeUUID);
    customer.addUniverseUUID(u2.universeUUID);
    customer.save();

    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("metric")));
    params.put("start", "1479281737");
    params.put("nodePrefix", "host-1");


    ArgumentCaptor<ArrayList> metricKeys = ArgumentCaptor.forClass(ArrayList.class);
    ArgumentCaptor<Map> queryParams = ArgumentCaptor.forClass(Map.class);
    route(fakeRequest("POST", baseRoute + customer.uuid + "/metrics").cookie(validCookie).bodyJson(params));
    verify(mockMetricQueryHelper).query((List<String>) metricKeys.capture(),
      (Map<String, String>) queryParams.capture());
    assertThat(queryParams.getValue(), is(notNullValue()));
    JsonNode filters = Json.parse(queryParams.getValue().get("filters").toString());
    assertThat(filters.get("table_name"), nullValue());
  }


  @Test
  public void testCustomerMetricsExceptionThrown() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("metric")));
    params.put("start", "1479281737");

    ObjectNode response = Json.newObject();
    response.put("error", "something went wrong");

    when(mockMetricQueryHelper.query(anyList(), anyMap())).thenThrow(new RuntimeException("Weird Data provided"));
    Result result = route(fakeRequest("POST", baseRoute + customer.uuid + "/metrics").cookie(validCookie).bodyJson(params));
    assertEquals(BAD_REQUEST, result.status());
    assertThat(contentAsString(result), allOf(notNullValue(), containsString("{\"error\":\"Weird Data provided\"}")));
  }

  @Test
  public void testCustomerMetricsWithMultipleUniverses() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    Universe u1 = createUniverse("Foo-1", customer.getCustomerId());
    u1 = Universe.saveDetails(u1.universeUUID, ApiUtils.mockUniverseUpdater("host-a"));
    Universe u2 = createUniverse("Foo-2", customer.getCustomerId());
    u2 = Universe.saveDetails(u2.universeUUID, ApiUtils.mockUniverseUpdater("host-b"));
    customer.addUniverseUUID(u1.universeUUID);
    customer.addUniverseUUID(u2.universeUUID);
    customer.save();

    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("metric")));
    params.put("start", "1479281737");

    ObjectNode response = Json.newObject();
    response.put("foo", "bar");
    ArgumentCaptor<ArrayList> metricKeys = ArgumentCaptor.forClass(ArrayList.class);
    ArgumentCaptor<Map> queryParams = ArgumentCaptor.forClass(Map.class);

    route(fakeRequest("POST", baseRoute + customer.uuid + "/metrics").cookie(validCookie).bodyJson(params));
    verify(mockMetricQueryHelper).query((List<String>) metricKeys.capture(),
      (Map<String, String>) queryParams.capture());

    assertThat(metricKeys.getValue(), is(notNullValue()));
    assertThat(queryParams.getValue(), is(notNullValue()));
    JsonNode filters = Json.parse(queryParams.getValue().get("filters").toString());
    String nodePrefix = filters.get("node_prefix").asText();
    assertThat(nodePrefix, allOf(notNullValue(), containsString("host-a")));
    assertThat(nodePrefix, allOf(notNullValue(), containsString("host-b")));
    String[] nodePrefixes = nodePrefix.split("\\|");
    assertEquals(nodePrefixes.length, 2);
  }

  @Test
  public void testCustomerMetricsWithNodePrefixParam() {
    String authToken = customer.createAuthToken();
    Http.Cookie validCookie = Http.Cookie.builder("authToken", authToken).build();
    Universe u1 = createUniverse("Foo-1", customer.getCustomerId());
    u1 = Universe.saveDetails(u1.universeUUID, ApiUtils.mockUniverseUpdater("host-1"));
    Universe u2 = createUniverse("Foo-2", customer.getCustomerId());
    u2 = Universe.saveDetails(u2.universeUUID, ApiUtils.mockUniverseUpdater("host-2"));
    customer.addUniverseUUID(u1.universeUUID);
    customer.addUniverseUUID(u2.universeUUID);
    customer.save();

    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("metric")));
    params.put("start", "1479281737");
    params.put("nodePrefix", "host-1");

    ArgumentCaptor<ArrayList> metricKeys = ArgumentCaptor.forClass(ArrayList.class);
    ArgumentCaptor<Map> queryParams = ArgumentCaptor.forClass(Map.class);
    route(fakeRequest("POST", baseRoute + customer.uuid + "/metrics").cookie(validCookie).bodyJson(params));
    verify(mockMetricQueryHelper).query((List<String>) metricKeys.capture(),
      (Map<String, String>) queryParams.capture());
    assertThat(queryParams.getValue(), is(notNullValue()));
    JsonNode filters = Json.parse(queryParams.getValue().get("filters").toString());
    String nodePrefix = filters.get("node_prefix").asText();
    assertThat(nodePrefix, allOf(notNullValue(), equalTo("host-1")));
  }

  @Test
  public void testCustomerMetricsWithNodeNameParam() {
    String authToken = customer.createAuthToken();

    Universe u1 = createUniverse("Foo-1", customer.getCustomerId());
    u1 = Universe.saveDetails(u1.universeUUID, ApiUtils.mockUniverseUpdater("host-1"));
    customer.addUniverseUUID(u1.universeUUID);
    customer.save();
    ObjectNode params = Json.newObject();
    params.set("metrics", Json.toJson(ImmutableList.of("metric")));
    params.put("start", "1479281737");
    params.put("nodePrefix", "host-1");
    params.put("nodeName", "host-n1");
    ArgumentCaptor<ArrayList> metricKeys = ArgumentCaptor.forClass(ArrayList.class);
    ArgumentCaptor<Map> queryParams = ArgumentCaptor.forClass(Map.class);
    FakeApiHelper.doRequestWithAuthTokenAndBody("POST", baseRoute + customer.uuid + "/metrics", authToken, params);
    verify(mockMetricQueryHelper).query((List<String>) metricKeys.capture(),
      (Map<String, String>) queryParams.capture());
    assertThat(queryParams.getValue(), is(notNullValue()));
    JsonNode filters = Json.parse(queryParams.getValue().get("filters").toString());
    String nodeName = filters.get("exported_instance").asText();
    assertThat(nodeName, allOf(notNullValue(), equalTo("host-n1")));
  }

  private Result getHostInfo(UUID customerUUID) {
    String uri = baseRoute + customerUUID + "/host_info";
    return FakeApiHelper.doRequestWithAuthToken("GET", uri, customer.createAuthToken());
  }

  @Test
  public void testCustomerHostInfoWithInvalidCustomer() {
    UUID randomUUID = UUID.randomUUID();
    Result result = getHostInfo(randomUUID);
    assertEquals(FORBIDDEN, result.status());

    String resultString = contentAsString(result);
    assertThat(resultString, allOf(notNullValue(),
        equalTo("Unable To Authenticate Customer")));
  }

  @Test
  public void testCustomerHostInfo() {
    JsonNode response = Json.parse("{\"foo\": \"bar\"}");
    when(mockCloudQueryHelper.currentHostInfo(Common.CloudType.aws,
      ImmutableList.of("instance-id", "vpc-id", "privateIp", "region"))).thenReturn(response);
    when(mockCloudQueryHelper.currentHostInfo(Common.CloudType.gcp, null)).thenReturn(response);
    Result result = getHostInfo(customer.uuid);
    JsonNode json = Json.parse(contentAsString(result));
    assertEquals(OK, result.status());
    ObjectNode responseNode = Json.newObject();
    responseNode.put("aws", response);
    responseNode.put("gcp", response);
    assertEquals(json, responseNode);
  }
}
