---
title: Deploy on Microsoft Azure using Terraform
headerTitle: Microsoft Azure
linkTitle: Microsoft Azure
description: Use Terraform to deploy YugabyteDB on Microsoft Azure.
menu:
  latest:
    identifier: deploy-in-azure-3-terraform
    parent: public-clouds
    weight: 650
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/deploy/public-clouds/azure/azure-arm" class="nav-link">
      <i class="icon-shell"></i>
      Azure ARM template
    </a>
  </li>
  <li >
    <a href="/latest/deploy/public-clouds/azure/aks" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Azure Kubernetes Service (AKS)
    </a>
  </li>
  <li>
    <a href="/latest/deploy/public-clouds/azure/terraform" class="nav-link active">
      <i class="icon-shell"></i>
      Terraform
    </a>
  </li>
</ul>

## Prerequisites

1. Download and install [terraform](https://www.terraform.io/downloads.html).

2. Verify using the `terraform` command. You should see a help message that looks similar to t.

    ```sh
    $ terraform
    ```
    
    ```
    Usage: terraform [--version] [--help] <command> [args]
    ...
    Common commands:
        apply              Builds or changes infrastructure
        console            Interactive console for Terraform interpolations
        destroy            Destroy Terraform-managed infrastructure
        env                Workspace management
        fmt                Rewrites config files to canonical format
    ```

## 1. Create a terraform configuration file

Create a terraform configuration file named `yugabyte-db-config.tf` and add the following details to it. The terraform module can be found in the [terraform-azure-yugabyte](https://github.com/yugabyte/terraform-azure-yugabyte) GitHub repository.

```sh
provider "azurerm" {
  # Provide your Azure Creadentilals 
    subscription_id = "AZURE_SUBSCRIPTION_ID"
    client_id       = "AZURE_CLIENT_ID"
    client_secret   = "AZURE_CLIENT_SECRET"
    tenant_id       = "AZURE_TENANT_ID"
}

module "yugabyte-db-cluster" {
  # The source module used for creating AWS clusters.
  source = "github.com/Yugabyte/terraform-azure-yugabyte"

  # The name of the cluster to be created, change as per need.
  cluster_name = "test-cluster"

  # key pair.
  ssh_private_key = "SSH_PRIVATE_KEY_HERE"
  ssh_public_key = "SSH_PUBLIC_KEY_HERE"
  ssh_user = "SSH_USER_NAME_HERE"

  # The region name where the nodes should be spawned.
  region_name = "YOUR VPC REGION"

  # The name of resource group in which all Azure resource will be created. 
  resource_group = "test-yugabyte"

  # Replication factor.
  replication_factor = "3"

  # The number of nodes in the cluster, this cannot be lower than the replication factor.
  node_count = "3"
}
```

**NOTE:** To install terraform and configure it for Azure, see [Quickstart: Install and configure Terraform to provision Azure resources](https://docs.microsoft.com/en-gb/azure/virtual-machines/linux/terraform-install-configure)

## 2. Create a cluster

Init terraform first if you have not already done so.

```sh
$ terraform init
```

Now, run the following to create the instances and bring up the cluster.

```sh
$ terraform apply
```

Once the cluster is created, you can go to the URL `http://<node ip or dns name>:7000` to view the UI. You can find the node's IP or DNS address by running the following:

```sh
$ terraform state show azurerm_virtual_machine.Yugabyte-Node[0]
```

You can access the cluster UI by going to any of the following URLs.

You can check the state of the nodes at any point by running the following command.

```sh
$ terraform show
```

## 3. Verify resources created

The following resources are created by this module:

- `module.azure-yugabyte.azurerm_virtual_machine.Yugabyte-Node` The Azure VM instances.

For a cluster named `test-cluster`, the instances will be named `yugabyte-test-cluster-node-1`, `yugabyte-test-cluster-node-2`, `yugabyte-test-cluster-node-3`.

- `module.azure-yugabyte.azurerm_network_security_group.Yugabyte-SG` The security group that allows the various clients to access the YugabyteDB cluster.

For a cluster named `test-cluster`, this security group will be named `yugabyte-test-cluster-SG` with the ports 7000, 9000, 9042, 7100, 9200 and 6379 open to all other instances in the same security group.

- `module.azure-yugabyte.null_resource.create_yugabyte_universe` A local script that configures the newly created instances to form a new YugabyteDB universe.
- `module.azure-yugabyte.azurerm_network_interface.Yugabyte-NIC` The Azure network interface for VM instance.
  
For cluster named `test-cluster`, the network interface will be named `yugabyte-test-cluster-NIC-1`, `yugabyte-test-cluster-NIC-2`, `yugabyte-test-cluster-NIC-3`.

## 4. Destroy the cluster [optional]

To destroy what you just created, you can run the following command.

```sh
$ terraform destroy
```
