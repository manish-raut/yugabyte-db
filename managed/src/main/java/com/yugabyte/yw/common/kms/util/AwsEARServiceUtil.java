/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common.kms.util;

import com.amazonaws.services.identitymanagement.AmazonIdentityManagement;
import com.amazonaws.services.identitymanagement.AmazonIdentityManagementClientBuilder;
import com.amazonaws.services.identitymanagement.model.GetRoleRequest;
import com.amazonaws.services.identitymanagement.model.GetRoleResult;
import com.amazonaws.services.identitymanagement.model.Role;
import com.amazonaws.services.identitymanagement.model.User;
import com.amazonaws.services.kms.AWSKMS;
import com.amazonaws.services.kms.AWSKMSClientBuilder;
import com.amazonaws.services.kms.model.AliasListEntry;
import com.amazonaws.services.kms.model.CreateAliasRequest;
import com.amazonaws.services.kms.model.CreateKeyRequest;
import com.amazonaws.services.kms.model.CreateKeyResult;
import com.amazonaws.services.kms.model.DecryptRequest;
import com.amazonaws.services.kms.model.GenerateDataKeyWithoutPlaintextRequest;
import com.amazonaws.services.kms.model.KeyListEntry;
import com.amazonaws.services.kms.model.ListAliasesRequest;
import com.amazonaws.services.kms.model.ListAliasesResult;
import com.amazonaws.services.kms.model.ListKeysRequest;
import com.amazonaws.services.kms.model.ListKeysResult;
import com.amazonaws.services.securitytoken.AWSSecurityTokenService;
import com.amazonaws.services.securitytoken.AWSSecurityTokenServiceClientBuilder;
import com.amazonaws.services.securitytoken.AWSSecurityTokenServiceClient;
import com.amazonaws.services.securitytoken.model.GetCallerIdentityRequest;
import com.amazonaws.services.securitytoken.model.GetCallerIdentityResult;
import com.avaje.ebean.annotation.EnumValue;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.kms.util.KeyProvider;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import java.io.File;
import java.nio.ByteBuffer;
import java.util.Map;
import java.util.Properties;
import java.util.UUID;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.Application;
import play.api.Play;

public class AwsEARServiceUtil {
    private static final String CMK_POLICY = "default_cmk_policy.json";

    private static final Logger LOG = LoggerFactory.getLogger(AwsEARServiceUtil.class);

    public enum KeyType {
        @EnumValue("CMK")
        CMK,
        @EnumValue("DATA_KEY")
        DATA_KEY;
    }

    private enum CredentialType {
        @EnumValue("KMS_CONFIG")
        KMS_CONFIG,
        @EnumValue("CLOUD_PROVIDER")
        CLOUD_PROVIDER;
    }

    /**
     * Determines which source to retrieve AWS credentials from
     *
     * @param customerUUID
     * @return KMS_CONFIG if one exists, CLOUD_PROVIDER otherwise
     */
    private static CredentialType getCredentialType(UUID customerUUID) {
        CredentialType type = null;
        ObjectNode authConfig = EncryptionAtRestUtil.getAuthConfig(customerUUID, KeyProvider.AWS);
        Provider provider = Provider.get(customerUUID, CloudType.aws);
        Map<String, String> providerConfig = provider == null ? null : provider.getConfig();
        if (authConfig == null &&
                provider != null &&
                providerConfig.get("AWS_ACCESS_KEY_ID") != null &&
                providerConfig.get("AWS_SECRET_ACCESS_KEY") != null &&
                ((Region) provider.regions.toArray()[0]).code != null) {
            type = CredentialType.CLOUD_PROVIDER;
        } else if (authConfig != null) {
            type = CredentialType.KMS_CONFIG;
        } else {
            throw new RuntimeException("Could not find AWS credentials for AWS KMS integration");
        }
        return type;
    }

    /**
     * Tries to set AWS credential system properties if any exist in authConfig
     * for the specified customer
     *
     * @param customerUUID the customer the authConfig should be retrieved for
     */
    // Rely on the AWS credential provider chain
    // We set system properties as the first-choice option
    // To debug, a user can set AWS_ACCESS_KEY_ID, AWS_SECRET_KEY, AWS_REGION env vars
    // which will override any existing KMS configuration credentials
    // https://docs.aws.amazon.com/sdk-for-java/v1/developer-guide/credentials.html
    private static void setUserCredentials(UUID customerUUID) {
        Properties p = new Properties(System.getProperties());
        CredentialType credentialType = getCredentialType(customerUUID);
        switch (credentialType) {
            case KMS_CONFIG:
                ObjectNode authConfig = EncryptionAtRestUtil.getAuthConfig(
                        customerUUID,
                        KeyProvider.AWS
                );
                if (authConfig.get("AWS_ACCESS_KEY_ID") != null &&
                        authConfig.get("AWS_SECRET_ACCESS_KEY") != null &&
                        authConfig.get("AWS_REGION") != null) {
                    p.setProperty("aws.accessKeyId", authConfig.get("AWS_ACCESS_KEY_ID").asText());
                    p.setProperty(
                            "aws.secretKey",
                            authConfig.get("AWS_SECRET_ACCESS_KEY").asText()
                    );
                    p.setProperty("aws.region", authConfig.get("AWS_REGION").asText());
                }
                break;
            case CLOUD_PROVIDER:
                Provider provider = Provider.get(customerUUID, CloudType.aws);
                Map<String, String> config = provider.getConfig();
                String accessKeyId = config.get("AWS_ACCESS_KEY_ID");
                String secretAccessKey = config.get("AWS_SECRET_ACCESS_KEY");
                String region = ((Region) provider.regions.toArray()[0]).code;
                p.setProperty("aws.accessKeyId", accessKeyId);
                p.setProperty("aws.secretKey", secretAccessKey);
                p.setProperty("aws.region", region);
                break;
        }
        System.setProperties(p);
    }

    /**
     * Instantiates a AWS KMS client using the default credential provider chain
     *
     * @param customerUUID the customer that should have their AWS credentials retrieved
     * @return a AWS KMS client
     */
    public static AWSKMS getKMSClient(UUID customerUUID) {
        setUserCredentials(customerUUID);
        return AWSKMSClientBuilder.defaultClient();
    }

    /**
     * Instantiates an AWS IAM client using the default credential provider chain
     *
     * @param customerUUID the customer that should have their AWS credentials retrieved
     * @return a AWS IAM client
     */
    public static AmazonIdentityManagement getIAMClient(UUID customerUUID) {
        setUserCredentials(customerUUID);
        return AmazonIdentityManagementClientBuilder.defaultClient();
    }

    /**
     * Instantiates an AWS STS client using the default credential provider chain
     *
     * @param customerUUID the customer that should have their AWS credentials retrieved
     * @return a AWS STS client
     */
    public static AWSSecurityTokenService getSTSClient(UUID customerUUID) {
        setUserCredentials(customerUUID);
        return AWSSecurityTokenServiceClientBuilder.defaultClient();
    }

    /**
     * Reads the policy base from a JSON file
     *
     * @return the policy base
     */
    public static ObjectNode getPolicyBase() {
        ObjectNode policy = null;
        try {
            ObjectMapper mapper = new ObjectMapper();
            Application application = Play.current().injector().instanceOf(Application.class);
            policy = (ObjectNode) mapper.readTree(application.resourceAsStream(CMK_POLICY));
        } catch (Exception e) {
            String errMsg = "Error occurred retrieving default cmk policy base";
            LOG.error(errMsg, e);
        }
        return policy;
    }

    /**
     * Retrieve the current AWS IAM User
     *
     * @param customerUUID the customer associated to the AWS credentials used
     * @return an AWS IAM User
     */
    public static User getIAMUser(UUID customerUUID) {
        return AwsEARServiceUtil.getIAMClient(customerUUID).getUser().getUser();
    }

    /**
     * Binds a AWS KMS CMK policy statement field to a dynamically retrieved Amazon resource name
     *
     * @param statement the AWS KMS CMK policy
     * @param arn the arn to attach to the policy statement
     * @return a CMK policy statement with the inputted arn attached
     */
    private static ObjectNode bindArnToStatement(ObjectNode statement, String arn) {
        String principalField = "Principal";
        ObjectNode principal = (ObjectNode) statement.get(principalField);
        principal.put("AWS", arn);
        statement.set(principalField, principal);
        return statement;
    }

    /**
     * Bind dynamically retrieved arns to default cmk policy base
     *
     * @param policyBase is the base of the CMK policy
     * @param userArn the current user's arn
     * @param rootArn the root user arn of the account associated to the current user arn
     * @return a bound AWS KMS CMK policy
     */
    public static ObjectNode bindParamsToPolicyBase(
            ObjectNode policyBase,
            String userArn,
            String rootArn
    ) {
        String statementField = "Statement";
        ArrayNode statements = (ArrayNode) policyBase.get(statementField);
        statements.set(0, bindArnToStatement((ObjectNode) statements.get(0), rootArn));
        statements.set(1, bindArnToStatement((ObjectNode) statements.get(1), userArn));
        return (ObjectNode) policyBase.set(statementField, statements);
    }

    /**
     * Retrieves the identity of the AWS credentials being used by this integration
     *
     * @param customerUUID the customer who's credentials are being tested for identity
     * @return the current caller identity
     */
    private static GetCallerIdentityResult getCallerIdentity(UUID customerUUID) {
        AWSSecurityTokenService client = getSTSClient(customerUUID);
        GetCallerIdentityRequest req = new GetCallerIdentityRequest();
        return client.getCallerIdentity(req);
    }

    /**
     *
     * @param arn
     * @return
     */
    private static String getResourceNameFromArn(String arn) {
        return (arn.split(":")[5]).split("/")[1];
    }

    /**
     * Generates the YB default AWS KMS CMK policy
     *
     * @param customerUUID the customer associated to the AWS credentials
     * @return a String of the default policy
     */
    public static String generateDefaultPolicy(UUID customerUUID) {
        ObjectNode policyBase = getPolicyBase();
        GetCallerIdentityResult callerIdentity = getCallerIdentity(customerUUID);
        if (callerIdentity != null) {
            String credentialsArn = callerIdentity.getArn();
            if (credentialsArn.contains(":assumed-role/")) {
                credentialsArn = getRole(
                        customerUUID,
                        getResourceNameFromArn(credentialsArn)
                ).getArn();
            } else if (!credentialsArn.contains(":user/")) {
                throw new RuntimeException(
                        "Credentials provided are not associated to a user or role"
                );
            }
            String rootArn = String.format("arn:aws:iam::%s:root", callerIdentity.getAccount());
            return bindParamsToPolicyBase(policyBase, credentialsArn, rootArn).toString();
        } else {
            LOG.error("Could not get AWS caller identity from provided credentials");
            return null;
        }
    }

    /**
     * Creates a new AWS KMS alias to be able to search for the CMK by
     *
     * @param universeUUID will be the name of the alias
     * @param kId is the CMk that the alias should attach itself to
     * @param aliasName is the name of the alias attached to the CMK with kId
     */
    private static void createAlias(UUID customerUUID, String kId, String aliasName) {
        final String aliasNameBase = "alias/%s";
        final CreateAliasRequest req = new CreateAliasRequest()
                .withAliasName(aliasName)
                .withTargetKeyId(kId);
        AwsEARServiceUtil.getKMSClient(customerUUID).createAlias(req);
    }

    /**
     * Try to retreive an AWS KMS alias by its name
     *
     * @param customerUUID is the customer that the alias was created for
     * @param aliasName is the name of the alias in AWS
     * @return the alias if found, or null otherwise
     */
    public static AliasListEntry getAlias(UUID customerUUID, String aliasName) {
        ListAliasesRequest req = new ListAliasesRequest().withLimit(100);
        AliasListEntry uniAlias = null;
        boolean done = false;
        AWSKMS client = AwsEARServiceUtil.getKMSClient(customerUUID);
        while (!done) {
            ListAliasesResult result = client.listAliases(req);
            for (AliasListEntry alias : result.getAliases()) {
                if (alias.getAliasName().equals(aliasName)) {
                    uniAlias = alias;
                    done = true;
                    break;
                }
            }
            req.setMarker(result.getNextMarker());
            if (!result.getTruncated()) done = true;
        }
        return uniAlias;
    }

    /**
     * Queries AWS IAM for a role matching the inputted role name
     *
     * @param customerUUID the customer the KMS Config is associated to
     * @param roleName the name of the AWS IAM role being queried for
     * @return the role found
     */
    private static Role getRole(UUID customerUUID, String roleName) {
        GetRoleRequest req = new GetRoleRequest().withRoleName(roleName);
        GetRoleResult result = getIAMClient(customerUUID).getRole(req);
        return result.getRole();
    }

    /**
     * Retrieve the Amazon resource name (arn) of an AWS KMS CMK with id of cmkId
     *
     * @param customerUUID the customer that should have their credentials used to make AWS requests
     * @param cmkId the id of the AWS KMS CMK that should have it's arn retrieved
     * @return the arn of the CMK, or null if something goes wrong
     */
    public static String getCMKArn(UUID customerUUID, String cmkId) {
        String cmkArn = null;
        ListKeysRequest req = new ListKeysRequest().withLimit(1000);
        boolean done = false;
        AWSKMS client = AwsEARServiceUtil.getKMSClient(customerUUID);
        while (!done) {
            ListKeysResult result = client.listKeys(req);
            for (KeyListEntry key : result.getKeys()) {
                if (key.getKeyId().equals(cmkId)) {
                    cmkArn = key.getKeyArn();
                    done = true;
                    break;
                }
            }
            req.setMarker(result.getNextMarker());
            if (!result.getTruncated()) done = true;
        }
        return cmkArn;
    }

    /**
     * Decrypts the universe master key ciphertext blob into plaintext using the universe CMK
     *
     * @param customerUUID is the customer the universe belongs to
     * @param encryptedUniverseKey is the ciphertext blob of the universe master key
     * @return a plaintext byte array of the universe master key
     */
    public static byte[] decryptUniverseKey(UUID customerUUID, byte[] encryptedUniverseKey) {
        if (encryptedUniverseKey == null) return null;
        ByteBuffer encryptedKeyBuffer = ByteBuffer.wrap(encryptedUniverseKey);
        encryptedKeyBuffer.rewind();
        final DecryptRequest req = new DecryptRequest().withCiphertextBlob(encryptedKeyBuffer);
        ByteBuffer decryptedKeyBuffer = AwsEARServiceUtil
                .getKMSClient(customerUUID)
                .decrypt(req)
                .getPlaintext();
        decryptedKeyBuffer.rewind();
        byte[] decryptedUniverseKey = new byte[decryptedKeyBuffer.remaining()];
        decryptedKeyBuffer.get(decryptedUniverseKey);
        return decryptedUniverseKey;
    }

    /**
     * Generates a universe master key using the universe alias targetted CMK
     *
     * @param customerUUID the customer this is for
     * @param cmkId the Id of the CMK that the alias for this universe is pointing to
     * @param algorithm is the desired universe master key algorithm
     * @param keySize is the desired universe master key size
     * @return a ciphertext blob of the universe master key
     */
    public static byte[] generateDataKey(
            UUID customerUUID,
            String cmkId,
            String algorithm,
            int keySize
    ) {
        final String keySpecBase = "%s_%s";
        final GenerateDataKeyWithoutPlaintextRequest dataKeyRequest =
                new GenerateDataKeyWithoutPlaintextRequest()
                        .withKeyId(cmkId)
                        .withKeySpec(
                                String.format(keySpecBase, algorithm, Integer.toString(keySize))
                        );
        ByteBuffer encryptedKeyBuffer =
                AwsEARServiceUtil.getKMSClient(customerUUID)
                        .generateDataKeyWithoutPlaintext(dataKeyRequest)
                        .getCiphertextBlob();
        encryptedKeyBuffer.rewind();
        byte[] encryptedKeyBytes = new byte[encryptedKeyBuffer.remaining()];
        encryptedKeyBuffer.get(encryptedKeyBytes);
        return encryptedKeyBytes;
    }

    /**
     * Search for and create/retrieve a CMK with matching alias name
     *
     * @param customerUUID the customer associated to AWS credentials used
     * @param policy the user-provided custom CMK policy
     * @param description the description of the CMK (if creation is required)
     * @param aliasBaseName the name of the alias associated with/to be associated with the CMK
     * @return the CMK id
     */
    public static String createOrRetrieveCMKWithAlias(
            UUID customerUUID,
            String policy,
            String description,
            String aliasBaseName
    ) {
        String result = null;
        final String aliasName = generateAliasName(aliasBaseName);
        AliasListEntry existingAlias = getAlias(customerUUID, aliasName);
        if (existingAlias == null) {
            // Create the CMK
            CreateKeyRequest req = new CreateKeyRequest();
            if (description != null) req = req.withDescription(description);
            // Use user-provided CMK policy
            if (policy != null && policy.length() > 0) req = req.withPolicy(policy);
                // Use YB default CMK policy
            else req = req.withPolicy(AwsEARServiceUtil.generateDefaultPolicy(customerUUID));
            CreateKeyResult createResult = AwsEARServiceUtil
                    .getKMSClient(customerUUID)
                    .createKey(req);
            final String kId = createResult.getKeyMetadata().getKeyId();
            // Associate CMK with alias
            createAlias(customerUUID, kId, aliasName);
            result = kId;
        } else {
            result = existingAlias.getTargetKeyId();
        }
        return result;
    }

    /**
     * A method to prefix an alias base name with the proper format for AWS
     *
     * @param aliasName the name of the alias
     * @return the basename of the alias prefixed with 'alias/'
     */
    public static String generateAliasName(String aliasName) {
        final String aliasNameBase = "alias/%s";
        return String.format(aliasNameBase, aliasName);
    }
}
