/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor;
import com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor.CommandType;

public class SetKubernetesUniverseKey extends KubernetesTaskBase {
    public static final Logger LOG = LoggerFactory.getLogger(SetKubernetesUniverseKey.class);

    @Override
    public void run() {
        LOG.info("Started {} task.", getName());
        try {
            // Create the task list sequence.
            subTaskGroupQueue = new SubTaskGroupQueue(userTaskUUID);

            // Update the universe DB with the update to be performed and set the
            // 'updateInProgress' flag to prevent other updates from happening.
            lockUniverseForUpdate(taskParams().expectedUniverseVersion);

            // Enable encryption-at-rest with new key file contents
            createEnableEncryptionAtRestTask(taskParams().enableEncryptionAtRest)
                    .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

            createDisableEncryptionAtRestTask(taskParams().disableEncryptionAtRest)
                    .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

            // Update the universe model to reflect encryption is now enabled
            writeEncryptionIntentToUniverse(taskParams().enableEncryptionAtRest);

            // Marks the update of this universe as a success only if all the tasks before it succeeded.
            createMarkUniverseUpdateSuccessTasks()
                    .setSubTaskGroupType(SubTaskGroupType.ConfigureUniverse);

            // Run all the tasks.
            subTaskGroupQueue.run();
        } catch (Throwable t) {
            LOG.error("Error executing task {}, error='{}'", getName(), t.getMessage(), t);
            throw t;
        } finally {
            // Mark the update of the universe as done. This will allow future edits/updates to the
            // universe to happen.
            unlockUniverseForUpdate();
        }
        LOG.info("Finished {} task.", getName());
    }
}
