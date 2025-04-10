// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.controllers;

import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.stream.Collectors;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableList;
import com.google.inject.Inject;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.common.CallHomeManager;
import com.yugabyte.yw.common.CloudQueryHelper;
import com.yugabyte.yw.common.PlacementInfoUtil;
import com.yugabyte.yw.common.ReleaseManager;
import com.yugabyte.yw.forms.CustomerRegisterFormData;
import com.yugabyte.yw.forms.FeatureUpdateFormData;
import com.yugabyte.yw.forms.MetricQueryParams;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerConfig;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Universe;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Result;


public class CustomerController extends AuthenticatedController {

  public static final Logger LOG = LoggerFactory.getLogger(CustomerController.class);

  @Inject
  FormFactory formFactory;

  @Inject
  MetricQueryHelper metricQueryHelper;

  @Inject
  ReleaseManager releaseManager;

  @Inject
  CloudQueryHelper cloudQueryHelper;

  public Result index(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      ObjectNode responseJson = Json.newObject();
      responseJson.put("error", "Invalid Customer UUID:" + customerUUID);
      return badRequest(responseJson);
    }

    ObjectNode responseJson = (ObjectNode)Json.toJson(customer);
    CustomerConfig config = CustomerConfig.getAlertConfig(customerUUID);
    if (config != null) {
      responseJson.set("alertingData", config.data);
    }
    responseJson.put("callhomeLevel", CustomerConfig.getOrCreateCallhomeLevel(customerUUID).toString());

    responseJson.put("features", customer.getFeatures());

    return ok(responseJson);
  }

  public Result update(UUID customerUUID) {
    ObjectNode responseJson = Json.newObject();
    ObjectNode errorJson = Json.newObject();

    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      responseJson.put("error", "Invalid Customer UUID:" + customerUUID);
      return badRequest(responseJson);
    }

    Form<CustomerRegisterFormData> formData = formFactory.form(CustomerRegisterFormData.class).bindFromRequest();
    if (formData.hasErrors()) {
      responseJson.set("error", formData.errorsAsJson());
      return badRequest(responseJson);
    }

    boolean hasPassword = formData.get().password != null && !formData.get().password.isEmpty();
    boolean hasConfirmPassword = formData.get().confirmPassword != null &&
            !formData.get().confirmPassword.isEmpty();
    if (hasPassword && hasConfirmPassword) {
      if (!formData.get().password.equals(formData.get().confirmPassword)) {
        String errorMsg = "Both passwords must match!";
        errorJson.put("password", errorMsg);
        errorJson.put("confirmPassword", errorMsg);
        responseJson.set("error", errorJson);
        return badRequest(responseJson);
      }
      // Had password info that matched, should update user password!
      customer.setPassword(formData.get().password);
    } else if (hasPassword && !hasConfirmPassword) {
      String errorMsg = "Please re-enter password";
      errorJson.put("confirmPassword", errorMsg);
      responseJson.set("error", errorJson);
      return badRequest(responseJson);
    } else if (!hasPassword && hasConfirmPassword) {
      String errorMsg = "Please enter password";
      errorJson.put("password", errorMsg);
      responseJson.set("error", errorJson);
      return badRequest(responseJson);
    }

    CustomerConfig config = CustomerConfig.getAlertConfig(customerUUID);
    if (config == null && formData.get().alertingData != null) {
      config = CustomerConfig.createAlertConfig(
              customerUUID, Json.toJson(formData.get().alertingData));
    } else if (config != null && formData.get().alertingData != null) {
      config.data = Json.toJson(formData.get().alertingData);
      config.update();
    }

    // Features would be a nested json, so we should fetch it differently.
    JsonNode requestBody = request().body().asJson();
    if (requestBody.has("features")) {
      customer.upsertFeatures(requestBody.get("features"));
    }

    CustomerConfig.upsertCallhomeConfig(customerUUID, formData.get().callhomeLevel);

    customer.name = formData.get().name;
    customer.email = formData.get().email;
    customer.update();

    return ok(Json.toJson(customer));
  }

  public Result delete(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);

    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID:" + customerUUID);
    }

    if (customer.delete()) {
      ObjectNode responseJson = Json.newObject();
      responseJson.put("success", true);
      return ApiResponse.success(responseJson);
    } else {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Unable to delete Customer UUID: " + customerUUID);
    }
  }

  public Result upsertFeatures(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID:" + customerUUID);
    }

    JsonNode requestBody = request().body().asJson();
    ObjectMapper mapper = new ObjectMapper();
    FeatureUpdateFormData formData;
    try {
      formData = mapper.treeToValue(requestBody, FeatureUpdateFormData.class);
    } catch (RuntimeException | JsonProcessingException e) {
      return ApiResponse.error(BAD_REQUEST, "Invalid JSON");
    }

    customer.upsertFeatures(formData.features);
    return ok(customer.getFeatures());
  }

  public Result metrics(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);

    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID:" + customerUUID);
    }

    Form<MetricQueryParams> formData = formFactory.form(MetricQueryParams.class).bindFromRequest();

    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    Map<String, String> params = formData.data();

    // Given we have a limitation on not being able to rename the pod labels in
    // kubernetes cadvisor metrics, we try to see if the metric being queried is for
    // container or not, and use pod_name vs exported_instance accordingly.
    // Expect for container metrics, all the metrics would with node_prefix and exported_instance.
    boolean hasContainerMetric = formData.get().metrics.stream().anyMatch(s -> s.startsWith("container"));
    String universeFilterLabel = hasContainerMetric ? "namespace" : "node_prefix";
    String nodeFilterLabel = hasContainerMetric ? "pod_name" : "exported_instance";
    String containerLabel = "container_name";
    String pvcLabel = "persistentvolumeclaim";

    ObjectNode filterJson = Json.newObject();
    if (!params.containsKey("nodePrefix")) {
      String universePrefixes = customer.getUniverses().stream()
        .map((universe -> universe.getUniverseDetails().nodePrefix)).collect(Collectors.joining("|"));
      filterJson.put(universeFilterLabel, String.join("|", universePrefixes));
    } else {
      // Check if it is a kubernetes deployment.
      if (hasContainerMetric) {
        if (params.containsKey("nodeName")) {
          // Get the correct namespace by appending the zone if it exists.
          String[] nodeWithZone = params.remove("nodeName").split("_");
          filterJson.put(nodeFilterLabel, nodeWithZone[0]);
          // The pod name is of the format yb-<server>-<replica_num> and we just need the
          // container, which is yb-<server>.
          String containerName = nodeWithZone[0].substring(0, nodeWithZone[0].lastIndexOf("-"));
          String pvcName = String.format("(.*)-%s", nodeWithZone[0]);
          String completeNamespace = params.remove("nodePrefix");
          if (nodeWithZone.length == 2) {
             completeNamespace = String.format("%s-%s", completeNamespace,
                                                      nodeWithZone[1]);
          }
          filterJson.put(universeFilterLabel, completeNamespace);
          filterJson.put(containerLabel, containerName);
          filterJson.put(pvcLabel, pvcName);

        } else {
          // If no nodename, we need to figure out the correct regex for the namespace.
          // We get this by getting the correct universe and then checking that the
          // provider for that universe is multi-az or not.
          final String nodePrefix = params.remove("nodePrefix");
          String completeNamespace = nodePrefix;
          List<Universe> universes =  customer.getUniverses().stream()
            .filter(u -> u.getUniverseDetails().nodePrefix.equals(nodePrefix))
            .collect(Collectors.toList());
          Provider provider = Provider.get(UUID.fromString(
            universes.get(0).getUniverseDetails().getPrimaryCluster().userIntent.provider));
          if (PlacementInfoUtil.isMultiAZ(provider)) {
            completeNamespace = String.format("%s-(.*)", completeNamespace);
          }
          filterJson.put(universeFilterLabel, completeNamespace);
        }
      } else {
        filterJson.put(universeFilterLabel, params.remove("nodePrefix"));
        if (params.containsKey("nodeName")) {
          filterJson.put(nodeFilterLabel, params.remove("nodeName"));
        }
      }
    }
    if (params.containsKey("tableName")) {
      filterJson.put("table_name", params.remove("tableName"));
    }
    params.put("filters", Json.stringify(filterJson));
    try {
      JsonNode response = metricQueryHelper.query(formData.get().metrics, params);
      if (response.has("error")) {
        return ApiResponse.error(BAD_REQUEST, response.get("error"));
      }
      return ApiResponse.success(response);
    } catch (RuntimeException e) {
      return ApiResponse.error(BAD_REQUEST, e.getMessage());
    }
  }

  public Result getHostInfo(UUID customerUUID) {
    Customer customer = Customer.get(customerUUID);
    if (customer == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    ObjectNode hostInfo = Json.newObject();
    hostInfo.put(Common.CloudType.aws.name(), cloudQueryHelper.currentHostInfo(
        Common.CloudType.aws, ImmutableList.of("instance-id", "vpc-id", "privateIp", "region")));
    hostInfo.put(Common.CloudType.gcp.name(), cloudQueryHelper.currentHostInfo(
        Common.CloudType.gcp, null));

    return ApiResponse.success(hostInfo);
  }
}
