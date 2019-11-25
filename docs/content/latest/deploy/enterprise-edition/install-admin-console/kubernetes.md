## Prerequisites

You must have a Kubernetes cluster that has [Helm](https://helm.sh/) configured. If you have not installed Helm client and server (aka Tiller) yet, follow the instructions [here](https://docs.helm.sh/using_helm/#installing-helm).

The YugaWare Helm chart documented here has been tested with the following software versions:

- Kubernetes 1.10+
- Helm 2.8.0+
- Yugabyte repository
- Kubernetes node with minimum 4 CPU core and 15 GB RAM can be allocated to YugaWare.

Confirm that your `helm` is configured correctly.

```sh
$ helm version
```

```
Client: &version.Version{SemVer:"v2.10.0", GitCommit:"...", GitTreeState:"clean"}
Server: &version.Version{SemVer:"v2.10.0", GitCommit:"...", GitTreeState:"clean"}
```

## Create a cluster

### Create a service account with cluster admin access

For deploying a YugaWare helm chart we need have a service account which has cluster admin access, if the user in context already has that access you can skip this step.

```sh
$ kubectl apply -f https://raw.githubusercontent.com/yugabyte/charts/master/stable/yugabyte/yugabyte-rbac.yaml
```

```sh
serviceaccount/yugabyte-helm created
clusterrolebinding.rbac.authorization.k8s.io/yugabyte-helm created
```

### Initialize Helm

Initialize `helm` with the service account but use the `--upgrade` flag to ensure that you can upgrade any previous initializations you may have made.

```sh
$ helm init --service-account yugabyte-helm --upgrade --wait
```

```sh
$HELM_HOME has been configured at /Users/<user>/.helm.

Tiller (the Helm server-side component) has been upgraded to the current version.
Happy Helming!
```

### Add yugaware repo
 Add yugabytedb repo which has both yugabyte and yugaware charts

You can do this as shown below.

```sh
$ helm repo add yugabytedb https://charts.yugabyte.com
```

### Fetch updates from the repository

```sh
$ helm repo update
```

### Validate the chart version

```sh
$ helm search yugabytedb/yugaware
```

```sh
NAME               	CHART VERSION	APP VERSION	  DESCRIPTION                                                 
yugabytedb/yugaware	2.0.4        	2.0.4.0-b7 	  YugaWare is YugaByte Database's Orchestration and Managem...
```

### Apply the secret 
Apply the secret file (license for yugaware) which you have received from the sales team
```sh
kubectl apply -f yugabyte-secret.yml -n yw-demo
```

### Install YugaWare

Install YugaWare in the Kubernetes cluster using the command below.

```sh
$ helm install yugabytedb/yugaware --namespace yw-demo --name yw-demo --wait
```

### Check the cluster status

You can check the status of the cluster using various commands noted below.

```sh
$ helm status yw-demo
```

```sh
LAST DEPLOYED: Thu Nov 21 18:43:41 2019
NAMESPACE: yw-demo
STATUS: DEPLOYED

RESOURCES:
==> v1/ClusterRole
NAME     AGE
yw-demo  18h

==> v1/ClusterRoleBinding
NAME     AGE
yw-demo  18h

==> v1/ConfigMap
NAME                                AGE
yw-demo-yugaware-app-config         18h
yw-demo-yugaware-global-config      18h
yw-demo-yugaware-nginx-config       18h
yw-demo-yugaware-prometheus-config  18h

==> v1/PersistentVolumeClaim
NAME                      AGE
yw-demo-yugaware-storage  18h

==> v1/Pod(related)
NAME                AGE
yw-demo-yugaware-0  144m

==> v1/Service
NAME                 AGE
yw-demo-yugaware-ui  18h

==> v1/ServiceAccount
NAME     AGE
yw-demo  18h

==> v1/StatefulSet
NAME              AGE
yw-demo-yugaware  18h
```

Get service details.

```sh
$ kubectl get svc -lapp=yw-demo-yugaware -n yw-demo
```

```sh
NAME                  TYPE           CLUSTER-IP   EXTERNAL-IP   PORT(S)                       AGE
yw-demo-yugaware-ui   LoadBalancer   10.0.18.5    34.68.1.65    80:30281/TCP,9090:32198/TCP   18h
```

You can even check the history of the `yw-demo` helm chart.

```sh
$ helm history yw-demo
```

```sh
REVISION	  UPDATED                 	STATUS  	CHART         	APP VERSION	 DESCRIPTION     
1       	Thu Nov 21 18:43:41 2019	DEPLOYED	yugaware-2.0.4	2.0.4.0-b7 	 Install complete
```

### Upgrade YugaWare

```sh
$ helm upgrade yw-demo yugabytedb/yugaware --set Image.tag=2.0.5.2 --wait
```

### Delete YugaWare

```sh
$ helm delete yw-demo --purge
```
