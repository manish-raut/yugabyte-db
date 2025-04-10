## Before you begin

- YugabyteDB is installed and ready for use. If you are new to YugabyteDB, you can create a local YugabyteDB cluster in under five minutes by following the steps in the [Quick start](../../quick-start/install/).

- Verify that you have the required extra loopback addresses by reviewing the Configure section.

- For the tutorial, use the default database `yugabyte` and the default user `yugabyte`.

## 1. Create two "data centers"

Create and start your first local cluster that will simulate "Data Center - East" by running the following `yb-ctl create` command from your YugabyteDB home directory.

```sh
$ ./bin/yb-ctl create --data_dir /Users/yugabyte_user/yugabyte/yb-datacenter-east --ip_start 1
```

This will start up a 1-node local cluster using the IP address of `127.0.0.1:7100` and create `yb-datacenter-east` as the data directory. Upon starting, you should see a screen like the following.

```
Creating cluster.
Waiting for cluster to be ready.
----------------------------------------------------------------------------------------------------
| Node Count: 1 | Replication Factor: 1                                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/postgres                                  |
| YSQL Shell          : bin/ysqlsh                                                                 |
| YCQL Shell          : bin/cqlsh                                                                  |
| YEDIS Shell         : bin/redis-cli                                                              |
| Web UI              : http://127.0.0.1:7000/                                                     |
| Cluster Data        : /Users/yugabyte_user/yugabyte/yb-datacenter-east                           |
----------------------------------------------------------------------------------------------------
```

Create and start your second local cluster that will simulate "Data Center = West" by running the following `yb-ctl create` command from your YugabyteDB home directory.

```sh
$ ./bin/yb-ctl create --data_dir yb-datacenter-west --ip_start 2
```

This will start up a 1-node cluster using IP address of `127.0.0.2` and create `yb-datacenter-west` as the data directory. Upon starting, you should see a screen like the following.

```
Creating cluster.
Waiting for cluster to be ready.
----------------------------------------------------------------------------------------------------
| Node Count: 1 | Replication Factor: 1                                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.2:5433/postgres                                  |
| YSQL Shell          : bin/ysqlsh -h 127.0.0.2                                                    |
| YCQL Shell          : bin/cqlsh 127.0.0.2                                                        |
| YEDIS Shell         : bin/redis-cli -h 127.0.0.2                                                 |
| Web UI              : http://127.0.0.2:7000/                                                     |
| Cluster Data        : /Users/yugabyte_user/yugabyte/yb-datacenter-west                           |
----------------------------------------------------------------------------------------------------
```

## 2. Create database tables

In the default `yugabyte` database, create the database table `users` on the "Data Center - East" cluster.

Open `ysqlsh` specifying the host IP address of `127.0.0.1`.

```sh
$ ./bin/ysqlsh -h 127.0.0.1
```

Run the following `CREATE TABLE` statement.

```postgresql
CREATE TABLE users (
    email varchar(35) PRIMARY KEY,
    username varchar(20)
    );
```

Now create the identical database table on cluster B.

Open `ysqlsh` for "Data Center - West" by specifying the host IP address of `127.0.0.2`.

```sh
$ ./bin/ysqlsh -h 127.0.0.2
```

Run the following `CREATE TABLE` statement.

```postgresql
CREATE TABLE users (
    email varchar(35) PRIMARY KEY,
    username varchar(20)
    );
```

You now have the identical database table on each of your clusters and can now set up 2DC asynchronous replication.

## 3. Configure unidirectional replication

To configure "Data Center - West" to be the consumer of data changes from the "Data Center - East" cluster, you need to use the `yb-admin` `setup_universe_replication` command. Review the syntax and then you can run the command.

```sh
yb-admin -master_addresses <consumer-master-addresses> setup_universe_replication <producer-universe_uuid> <producer_master_addresses> <producer-table-ids>
```

- *consumer-master-addresses*: a comma-separated list of the YB-Master services. For this simulation, you have one YB-Master service for each cluster (typically, there are three).
- *producer-universe-uuid*: a unique identifier for the producer cluster. The UUID can be found in the YB-Master UI (`<yb-master-ip>:7000`).
- *producer-table-ids*: A comma-separated list of `table_id` values (the generated UUIDs can be found in the YB-Master UI (`<yb-master-ip>:7000`).

Based on your actual values (which you got from the YB-Master UI page (`yb-master-ip>:7000`)), run the `yb-admin` `setup_universe_replication` command like in this example.

*consumer-master-addresses*: `127.0.0.2:7100`
*producer-universe-uuid*: `7acd6399-657d-42dc-a90a-646869898c2d`
*producer-master-addresses*: `127.0.0.1:7100`
*producer-table-ids*: `000030a9000030008000000000004000`

```sh
yb-admin -master_addresses 127.0.0.2:7100 setup_universe_replication 7acd6399-657d-42dc-a90a-646869898c2d 127.0.0.1:7100 000030a9000030008000000000004000
```

You should see a message like the following:

```
I1113 12:02:46.443101 136273344 mem_tracker.cc:250] MemTracker: hard memory limit is 13.600000 GB
I1113 12:02:46.443816 136273344 mem_tracker.cc:252] MemTracker: soft memory limit is 11.560000 GB
Replication setup successfully
```

## 4. Verify unidirectional replication

Now that you've configured unidirectional replication, you can now add data to the `users` table on the "Data Center - West" cluster and see the data appear in the `users` table on "Data Center - East" cluster. 

To add data to the "Data Center - East" cluster, open the YSQL shell (`ysqlsh`) by running the following command, making sure you are pointing to the new producer host.

```sh
$ ./bin/ysqlsh -host 127.0.0.1
```

On the "producer" cluster, open the YSQL shell (`ysqlsh`) and run the following to quickly see that data has been replicated between clusters.

```sh
$ ./bin/ysqlsh -host 127.0.0.2
```

```postgresql
yugabyte=# INSERT INTO users(email, username) VALUES ('hector@example.com', 'hector'), ('steve@example.com', 'steve');
```

```postgresql
yugabyte=# SELECT * FROM users;
```

You should see the following in the results.

```
       email         | username
---------------------+----------
 hector@example.com  | hector
 steve@example.com   | steve
(2 rows)
```

## 5. Configure bidirectional replication (optional)

Bidirectional asynchronous replication lets you insert data into the same table on either of the clusters and have the data changes added to the other cluster.

To configure bidirectional asynchronous replication for the same table, you need to run the following `yb-admin` `setup_universe_replication` command to set up the "Data Center - East" cluster to be the consumer of the "Data Center - West" cluster. For this example, here are the values used and the command example.

*consumer-master-addresses*: `127.0.0.1:7100`
*producer-universe-uuid*: `0a315687-e9bd-430f-b6f4-ac831193a394`
*producer-master-addresses*: `127.0.0.2:7100`
*producer-table-ids*: `000030a9000030008000000000004000`

```sh
$ ./bin/yb-admin -master_addresses 127.0.0.1:7100 setup_universe_replication 0a315687-e9bd-430f-b6f4-ac831193a394  127.0.0.2:7100 000030a9000030008000000000004000
```

You should see a message that shows the following:

```
Replication setup successfully
```

## 6. Verify bidirectional replication (optional)

Now that you've configured bidirectional replication, you can now add data to the `users` table on the "Data Center - West" cluster and see the data appear in the `users` table on "Data Center - East" cluster.

To add data to the "Data Center - West" cluster, open the YSQL shell (`ysqlsh`) by running the following command, making sure you are pointing to the new producer host.

```sh
$ ./bin/ysqlsh -host 127.0.0.2
```

On the new "consumer" cluster, open the YSQL shell (`ysqlsh`) and run the following to quickly see that data has been replicated between clusters.

```sh
$ ./bin/ysqlsh -host 127.0.0.1
```

```postgresql
yugabyte=# INSERT INTO users(email, username) VALUES ('neha@example.com', 'neha'), ('mikhail@example.com', 'mikhail');
```

```postgresql
yugabyte=# SELECT * FROM users;
```

You should see the following in the results.

```
       email         | username
---------------------+----------
 hector@example.com  | hector
 steve@example.com   | steve
 neha@example.com    | neha
 mikhail@example.com | mikhail
(4 rows)
```

## 7. Clean up

At this point, you've finished the tutorial and can either stop and save your examples or destroy and remove the clusters and their associated data directories.

To stop the simulated "data centers", use the `yb-ctl stop` commands using the `--data_dir` option to specify the cluster.

**Example - stopping "Data Center - East"** 

```sh
$ ./bin/yb-ctl stop --data_dir /Users/yugabyte_user/yugabyte/yb-datacenter-east
```

To destroy the simulated "data centers" and remove its associate data directory, use the `yb-ctl destroy` command with the `--data_dir` option to specify the cluster.

**Example — destroying and removing the "Data Center - West"**

```sh
$ ./bin/yb-ctl stop --data_dir /Users/yugabyte_user/yugabyte/yb-datacenter-west
```

## What's next

For more information, see the following in the Architecture section:

- [Two data center (2DC) deployments](../../architecture/2dc-deployments/)
- [Change data capture (CDC)](../../architecture/cdc-architecture)

For detailed design documents, see the following GitHub repository pages:

- [Two Data Center Deployment Support with YugaByte DB](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/multi-region-2DC-deployment.md)
- [Change Data Capture in YugabyteDB](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-change-data-capture.md)
