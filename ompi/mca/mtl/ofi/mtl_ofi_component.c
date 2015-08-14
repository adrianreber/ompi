/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2013-2015 Intel, Inc. All rights reserved
 *
 * Copyright (c) 2014-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "mtl_ofi.h"

static int ompi_mtl_ofi_component_open(void);
static int ompi_mtl_ofi_component_query(mca_base_module_t **module, int *priority);
static int ompi_mtl_ofi_component_close(void);
static int ompi_mtl_ofi_component_register(void);

static mca_mtl_base_module_t*
ompi_mtl_ofi_component_init(bool enable_progress_threads,
                            bool enable_mpi_threads);

static int param_priority;

mca_mtl_ofi_component_t mca_mtl_ofi_component = {
    {

        /* First, the mca_base_component_t struct containing meta
         * information about the component itself */

        .mtl_version = {
            MCA_MTL_BASE_VERSION_2_0_0,

            .mca_component_name = "ofi",
            OFI_COMPAT_MCA_VERSION,
            .mca_open_component = ompi_mtl_ofi_component_open,
            .mca_close_component = ompi_mtl_ofi_component_close,
            .mca_query_component = ompi_mtl_ofi_component_query,
            .mca_register_component_params = ompi_mtl_ofi_component_register,
        },
        .mtl_data = {
            /* The component is not checkpoint ready */
            MCA_BASE_METADATA_PARAM_NONE
        },

        .mtl_init = ompi_mtl_ofi_component_init,
    }
};

static int
ompi_mtl_ofi_component_register(void)
{
    ompi_mtl_ofi.provider_name = NULL;
    (void) mca_base_component_var_register(&mca_mtl_ofi_component.super.mtl_version,
                                           "provider",
                                           "Name of OFI provider to use",
                                           MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                           OPAL_INFO_LVL_4,
                                           MCA_BASE_VAR_SCOPE_READONLY,
                                           &ompi_mtl_ofi.provider_name);
    param_priority = 10;   /* for now give a lower priority than the psm mtl */
    mca_base_component_var_register (&mca_mtl_ofi_component.super.mtl_version,
                                     "priority", "Priority of the OFI MTL component",
                                      MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                      OPAL_INFO_LVL_9,
                                      MCA_BASE_VAR_SCOPE_READONLY,
                                      &param_priority);
    return OMPI_SUCCESS;
}



static int
ompi_mtl_ofi_component_open(void)
{
    ompi_mtl_ofi.base.mtl_request_size =
        sizeof(ompi_mtl_ofi_request_t) - sizeof(struct mca_mtl_request_t);

    ompi_mtl_ofi.domain =  NULL;
    ompi_mtl_ofi.av     =  NULL;
    ompi_mtl_ofi.cq     =  NULL;
    ompi_mtl_ofi.ep     =  NULL;

    return OMPI_SUCCESS;
}

static int
ompi_mtl_ofi_component_query(mca_base_module_t **module, int *priority)
{
    *priority = param_priority;
    *module = (mca_base_module_t *)&ompi_mtl_ofi.base;
    return OMPI_SUCCESS;
}

static int
ompi_mtl_ofi_component_close(void)
{
    return OMPI_SUCCESS;
}

int
ompi_mtl_ofi_progress_no_inline(void)
{
	return ompi_mtl_ofi_progress();
}

static mca_mtl_base_module_t*
ompi_mtl_ofi_component_init(bool enable_progress_threads,
                            bool enable_mpi_threads)
{
    int ret, fi_version;
    struct fi_info *hints;
    struct fi_info *providers = NULL, *prov = NULL;
    struct fi_cq_attr cq_attr = {0};
    struct fi_av_attr av_attr = {0};
    char ep_name[FI_NAME_MAX] = {0};
    size_t namelen;

    /**
     * Hints to filter providers
     * See man fi_getinfo for a list of all filters
     * mode:  Select capabilities MTL is prepared to support.
     *        In this case, MTL will pass in context into communication calls
     * ep_type:  reliable datagram operation
     * caps:     Capabilities required from the provider.
     *           Tag matching is specified to implement MPI semantics.
     */
    hints = fi_allocinfo();
    if (!hints) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: Could not allocate fi_info\n",
                            __FILE__, __LINE__);
        goto error;
    }
    hints->mode             = FI_CONTEXT;
    hints->ep_attr->type    = FI_EP_RDM;      /* Reliable datagram         */
    hints->caps             = FI_TAGGED;      /* Tag matching interface    */

    /**
     * Refine filter for additional capabilities
     * threading:  Disable locking
     * control_progress:  enable async progress
     */
    hints->domain_attr->threading        = FI_THREAD_ENDPOINT;
    hints->domain_attr->control_progress = FI_PROGRESS_AUTO;
    if (NULL != ompi_mtl_ofi.provider_name) {
        hints->fabric_attr->prov_name = strdup(ompi_mtl_ofi.provider_name);
    } else {
        hints->fabric_attr->prov_name = NULL;
    }

    /**
     * FI_VERSION provides binary backward and forward compatibility support
     * Specify the version of OFI is coded to, the provider will select struct
     * layouts that are compatible with this version.
     */
    fi_version = FI_VERSION(1, 0);

    /**
     * fi_getinfo:  returns information about fabric  services for reaching a
     * remote node or service.  this does not necessarily allocate resources.
     * Pass NULL for name/service because we want a list of providers supported.
     */
    ret = fi_getinfo(fi_version,    /* OFI version requested                    */
                     NULL,          /* Optional name or fabric to resolve       */
                     NULL,          /* Optional service name or port to request */
                     0ULL,          /* Optional flag                            */
                     hints,        /* In: Hints to filter providers            */
                     &providers);   /* Out: List of matching providers          */
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_getinfo failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Here we elect to use the first provider from the list.
     * Further filtering could be done at this point (e.g. name).
     */
    prov = providers;

    /**
     * Open fabric
     * The getinfo struct returns a fabric attribute struct that can be used to
     * instantiate the virtual or physical network. This opens a "fabric
     * provider". See man fi_fabric for details.
     */
    ret = fi_fabric(prov->fabric_attr,    /* In:  Fabric attributes             */
                    &ompi_mtl_ofi.fabric, /* Out: Fabric handle                 */
                    NULL);                /* Optional context for fabric events */
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_fabric failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Create the access domain, which is the physical or virtual network or
     * hardware port/collection of ports.  Returns a domain object that can be
     * used to create endpoints.  See man fi_domain for details.
     */
    ret = fi_domain(ompi_mtl_ofi.fabric,  /* In:  Fabric object                 */
                    prov,                 /* In:  Provider                      */
                    &ompi_mtl_ofi.domain, /* Out: Domain oject                  */
                    NULL);                /* Optional context for domain events */
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_domain failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Create a transport level communication endpoint.  To use the endpoint,
     * it must be bound to completion counters or event queues and enabled,
     * and the resources consumed by it, such as address vectors, counters,
     * completion queues, etc.
     * see man fi_endpoint for more details.
     */
    ret = fi_endpoint(ompi_mtl_ofi.domain, /* In:  Domain object   */
                      prov,                /* In:  Provider        */
                      &ompi_mtl_ofi.ep,    /* Out: Endpoint object */
                      NULL);               /* Optional context     */
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_endpoint failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Save the maximum inject size.
     */
    ompi_mtl_ofi.max_inject_size = prov->tx_attr->inject_size;

    /**
     * Create the objects that will be bound to the endpoint.
     * The objects include:
     *     - completion queue for events
     *     - address vector of other endpoint addresses
     *     - dynamic memory-spanning memory region
     */
    cq_attr.format = FI_CQ_FORMAT_TAGGED;
    ret = fi_cq_open(ompi_mtl_ofi.domain, &cq_attr, &ompi_mtl_ofi.cq, NULL);
    if (ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_cq_open failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * The remote fi_addr will be stored in the ofi_endpoint struct.
     * So, we use the AV in "map" mode.
     */
    av_attr.type = FI_AV_MAP;
    ret = fi_av_open(ompi_mtl_ofi.domain, &av_attr, &ompi_mtl_ofi.av, NULL);
    if (ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_av_open failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Bind the CQ and AV to the endpoint object.
     */
    ret = fi_ep_bind(ompi_mtl_ofi.ep,
                     (fid_t)ompi_mtl_ofi.cq,
                     FI_SEND | FI_RECV);
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_bind CQ-EP failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    ret = fi_ep_bind(ompi_mtl_ofi.ep,
                     (fid_t)ompi_mtl_ofi.av,
                     0);
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_bind AV-EP failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Enable the endpoint for communication
     * This commits the bind operations.
     */
    ret = fi_enable(ompi_mtl_ofi.ep);
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_enable failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Free providers info since it's not needed anymore.
     */
    fi_freeinfo(hints);
    hints = NULL;
    fi_freeinfo(providers);
    providers = NULL;

    /**
     * Get our address and publish it with modex.
     */
    namelen = sizeof(ep_name);
    ret = fi_getname((fid_t)ompi_mtl_ofi.ep, &ep_name[0], &namelen);
    if (ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_getname failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    OFI_COMPAT_MODEX_SEND(ret,
                          &mca_mtl_ofi_component.super.mtl_version,
                          &ep_name,
                          namelen);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: modex_send failed: %d\n",
                            __FILE__, __LINE__, ret);
        goto error;
    }

    ompi_mtl_ofi.epnamelen = namelen;

    /**
     * Set the ANY_SRC address.
     */
    ompi_mtl_ofi.any_addr = FI_ADDR_UNSPEC;

    /**
     * Activate progress callback.
     */
    ret = opal_progress_register(ompi_mtl_ofi_progress_no_inline);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: opal_progress_register failed: %d\n",
                            __FILE__, __LINE__, ret);
        goto error;
    }

    return &ompi_mtl_ofi.base;

error:
    if (providers) {
        (void) fi_freeinfo(providers);
    }
    if (hints) {
        (void) fi_freeinfo(hints);
    }
    if (ompi_mtl_ofi.av) {
        (void) fi_close((fid_t)ompi_mtl_ofi.av);
    }
    if (ompi_mtl_ofi.cq) {
        (void) fi_close((fid_t)ompi_mtl_ofi.cq);
    }
    if (ompi_mtl_ofi.ep) {
        (void) fi_close((fid_t)ompi_mtl_ofi.ep);
    }
    if (ompi_mtl_ofi.domain) {
        (void) fi_close((fid_t)ompi_mtl_ofi.domain);
    }
    if (ompi_mtl_ofi.fabric) {
        (void) fi_close((fid_t)ompi_mtl_ofi.fabric);
    }
    return NULL;
}

int
ompi_mtl_ofi_finalize(struct mca_mtl_base_module_t *mtl)
{
    opal_progress_unregister(ompi_mtl_ofi_progress_no_inline);

    /**
 *      * Close all the OFI objects
 *           */
    if (fi_close((fid_t)ompi_mtl_ofi.ep)) {
        opal_output(ompi_mtl_base_framework.framework_output,
                "fi_close failed: %s", strerror(errno));
        abort();
    }
    if (fi_close((fid_t)ompi_mtl_ofi.cq)) {
        opal_output(ompi_mtl_base_framework.framework_output,
                "fi_close failed: %s", strerror(errno));
        abort();
    }
    if (fi_close((fid_t)ompi_mtl_ofi.av)) {
        opal_output(ompi_mtl_base_framework.framework_output,
                "fi_close failed: %s", strerror(errno));
        abort();
    }
    if (fi_close((fid_t)ompi_mtl_ofi.domain)) {
        opal_output(ompi_mtl_base_framework.framework_output,
                "fi_close failed: %s", strerror(errno));
        abort();
    }
    if (fi_close((fid_t)ompi_mtl_ofi.fabric)) {
        opal_output(ompi_mtl_base_framework.framework_output,
                "fi_close failed: %s", strerror(errno));
        abort();
    }

    return OMPI_SUCCESS;
}



