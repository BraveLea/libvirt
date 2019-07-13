/*
 * virsh-completer.c: virsh completer callbacks
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "virsh-completer.h"
#include "virsh-domain.h"
#include "virsh.h"
#include "virsh-pool.h"
#include "virsh-nodedev.h"
#include "virsh-util.h"
#include "virsh-secret.h"
#include "virsh-network.h"
#include "internal.h"
#include "virutil.h"
#include "viralloc.h"
#include "virmacaddr.h"
#include "virstring.h"
#include "virxml.h"
#include "conf/node_device_conf.h"


/**
 * A completer callback is a function that accepts three arguments:
 *
 *   @ctl: virsh control structure
 *   @cmd: parsed input
 *   @flags: optional flags to alter completer's behaviour
 *
 * The @ctl contains connection to the daemon (should the
 * completer need it). Any completer that requires a connection
 * must check whether connection is still alive.
 *
 * The @cmd contains parsed user input which might be missing
 * some arguments (if user is still typing the command), but may
 * already contain important data. For instance if the completer
 * needs domain XML it may inspect @cmd to find --domain. Using
 * existing wrappers is advised. If @cmd does not contain all
 * necessary bits, completer might return sensible defaults (i.e.
 * generic values not tailored to specific use case) or return
 * NULL (i.e. no strings are offered to the user for completion).
 *
 * The @flags contains a .completer_flags value defined for each
 * use or 0 if no .completer_flags were specified. If a completer
 * is generic enough @flags can be used to alter its behaviour.
 * For instance, a completer to fetch names of domains can use
 * @flags to return names of only domains in a particular state
 * that the command accepts.
 *
 * Under no circumstances should a completer output anything.
 * Neither to stdout nor to stderr. This would harm the user
 * experience.
 */


/**
 * virshCommaStringListComplete:
 * @input: user input so far
 * @options: ALL options available for argument
 *
 * Some arguments to our commands accept the following form:
 *
 *   virsh command --arg str1,str2,str3
 *
 * This does not play nicely with our completer funtions, because
 * they have to return strings prepended with user's input. For
 * instance:
 *
 *   str1,str2,str3,strA
 *   str1,str2,str3,strB
 *   str1,str2,str3,strC
 *
 * This helper function takes care of that. In this specific case
 * it would be called as follows:
 *
 *   virshCommaStringListComplete("str1,str2,str3",
 *                                {"strA", "strB", "strC", NULL});
 *
 * Returns: string list of completions on success,
 *          NULL otherwise.
 */
char **
virshCommaStringListComplete(const char *input,
                             const char **options)
{
    const size_t optionsLen = virStringListLength(options);
    VIR_AUTOFREE(char *) inputCopy = NULL;
    VIR_AUTOSTRINGLIST inputList = NULL;
    VIR_AUTOSTRINGLIST ret = NULL;
    size_t nret = 0;
    size_t i;

    if (STREQ_NULLABLE(input, " "))
        input = NULL;

    if (input) {
        char *comma = NULL;

        if (VIR_STRDUP(inputCopy, input) < 0)
            return NULL;

        if ((comma = strrchr(inputCopy, ',')))
            *comma = '\0';
        else
            VIR_FREE(inputCopy);
    }

    if (inputCopy && !(inputList = virStringSplit(inputCopy, ",", 0)))
        return NULL;

    if (VIR_ALLOC_N(ret, optionsLen + 1) < 0)
        return NULL;

    for (i = 0; i < optionsLen; i++) {
        if (virStringListHasString((const char **)inputList, options[i]))
            continue;

        if ((inputCopy && virAsprintf(&ret[nret], "%s,%s", inputCopy, options[i]) < 0) ||
            (!inputCopy && VIR_STRDUP(ret[nret], options[i]) < 0))
            return NULL;

        nret++;
    }

    VIR_RETURN_PTR(ret);
}


char **
virshInterfaceNameCompleter(vshControl *ctl,
                            const vshCmd *cmd ATTRIBUTE_UNUSED,
                            unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virInterfacePtr *ifaces = NULL;
    int nifaces = 0;
    size_t i = 0;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(VIR_CONNECT_LIST_INTERFACES_ACTIVE |
                  VIR_CONNECT_LIST_INTERFACES_INACTIVE,
                  NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if ((nifaces = virConnectListAllInterfaces(priv->conn, &ifaces, flags)) < 0)
        return NULL;

    if (VIR_ALLOC_N(tmp, nifaces + 1) < 0)
        goto cleanup;

    for (i = 0; i < nifaces; i++) {
        const char *name = virInterfaceGetName(ifaces[i]);

        if (VIR_STRDUP(tmp[i], name) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, tmp);

 cleanup:
    for (i = 0; i < nifaces; i++)
        virInterfaceFree(ifaces[i]);
    VIR_FREE(ifaces);
    return ret;
}


char **
virshNetworkNameCompleter(vshControl *ctl,
                          const vshCmd *cmd ATTRIBUTE_UNUSED,
                          unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virNetworkPtr *nets = NULL;
    int nnets = 0;
    size_t i = 0;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(VIR_CONNECT_LIST_NETWORKS_INACTIVE |
                  VIR_CONNECT_LIST_NETWORKS_ACTIVE |
                  VIR_CONNECT_LIST_NETWORKS_PERSISTENT,
                  NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if ((nnets = virConnectListAllNetworks(priv->conn, &nets, flags)) < 0)
        return NULL;

    if (VIR_ALLOC_N(tmp, nnets + 1) < 0)
        goto cleanup;

    for (i = 0; i < nnets; i++) {
        const char *name = virNetworkGetName(nets[i]);

        if (VIR_STRDUP(tmp[i], name) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, tmp);

 cleanup:
    for (i = 0; i < nnets; i++)
        virNetworkFree(nets[i]);
    VIR_FREE(nets);
    return ret;
}


char **
virshNetworkEventNameCompleter(vshControl *ctl ATTRIBUTE_UNUSED,
                               const vshCmd *cmd ATTRIBUTE_UNUSED,
                               unsigned int flags)
{
    size_t i = 0;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (VIR_ALLOC_N(tmp, VIR_NETWORK_EVENT_ID_LAST + 1) < 0)
        goto cleanup;

    for (i = 0; i < VIR_NETWORK_EVENT_ID_LAST; i++) {
        if (VIR_STRDUP(tmp[i], virshNetworkEventCallbacks[i].name) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, tmp);

 cleanup:
    return ret;
}


char **
virshNetworkPortUUIDCompleter(vshControl *ctl,
                              const vshCmd *cmd ATTRIBUTE_UNUSED,
                              unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virNetworkPtr net = NULL;
    virNetworkPortPtr *ports = NULL;
    int nports = 0;
    size_t i = 0;
    char **ret = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if (!(net = virshCommandOptNetwork(ctl, cmd, NULL)))
        return false;

    if ((nports = virNetworkListAllPorts(net, &ports, flags)) < 0)
        return NULL;

    if (VIR_ALLOC_N(ret, nports + 1) < 0)
        goto error;

    for (i = 0; i < nports; i++) {
        char uuid[VIR_UUID_STRING_BUFLEN];

        if (virNetworkPortGetUUIDString(ports[i], uuid) < 0 ||
            VIR_STRDUP(ret[i], uuid) < 0)
            goto error;

        virNetworkPortFree(ports[i]);
    }
    VIR_FREE(ports);

    return ret;

 error:
    for (; i < nports; i++)
        virNetworkPortFree(ports[i]);
    VIR_FREE(ports);
    for (i = 0; i < nports; i++)
        VIR_FREE(ret[i]);
    VIR_FREE(ret);
    return NULL;
}


char **
virshNodeDeviceNameCompleter(vshControl *ctl,
                             const vshCmd *cmd ATTRIBUTE_UNUSED,
                             unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virNodeDevicePtr *devs = NULL;
    int ndevs = 0;
    size_t i = 0;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if ((ndevs = virConnectListAllNodeDevices(priv->conn, &devs, flags)) < 0)
        return NULL;

    if (VIR_ALLOC_N(tmp, ndevs + 1) < 0)
        goto cleanup;

    for (i = 0; i < ndevs; i++) {
        const char *name = virNodeDeviceGetName(devs[i]);

        if (VIR_STRDUP(tmp[i], name) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, tmp);

 cleanup:
    for (i = 0; i < ndevs; i++)
        virNodeDeviceFree(devs[i]);
    VIR_FREE(devs);
    return ret;
}


char **
virshNWFilterNameCompleter(vshControl *ctl,
                           const vshCmd *cmd ATTRIBUTE_UNUSED,
                           unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virNWFilterPtr *nwfilters = NULL;
    int nnwfilters = 0;
    size_t i = 0;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if ((nnwfilters = virConnectListAllNWFilters(priv->conn, &nwfilters, flags)) < 0)
        return NULL;

    if (VIR_ALLOC_N(tmp, nnwfilters + 1) < 0)
        goto cleanup;

    for (i = 0; i < nnwfilters; i++) {
        const char *name = virNWFilterGetName(nwfilters[i]);

        if (VIR_STRDUP(tmp[i], name) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, tmp);

 cleanup:
    for (i = 0; i < nnwfilters; i++)
        virNWFilterFree(nwfilters[i]);
    VIR_FREE(nwfilters);
    return ret;
}


char **
virshNWFilterBindingNameCompleter(vshControl *ctl,
                                  const vshCmd *cmd ATTRIBUTE_UNUSED,
                                  unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virNWFilterBindingPtr *bindings = NULL;
    int nbindings = 0;
    size_t i = 0;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if ((nbindings = virConnectListAllNWFilterBindings(priv->conn, &bindings, flags)) < 0)
        return NULL;

    if (VIR_ALLOC_N(tmp, nbindings + 1) < 0)
        goto cleanup;

    for (i = 0; i < nbindings; i++) {
        const char *name = virNWFilterBindingGetPortDev(bindings[i]);

        if (VIR_STRDUP(tmp[i], name) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, tmp);

 cleanup:
    for (i = 0; i < nbindings; i++)
        virNWFilterBindingFree(bindings[i]);
    VIR_FREE(bindings);
    return ret;
}


char **
virshSecretUUIDCompleter(vshControl *ctl,
                         const vshCmd *cmd ATTRIBUTE_UNUSED,
                         unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virSecretPtr *secrets = NULL;
    int nsecrets = 0;
    size_t i = 0;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if ((nsecrets = virConnectListAllSecrets(priv->conn, &secrets, flags)) < 0)
        return NULL;

    if (VIR_ALLOC_N(tmp, nsecrets + 1) < 0)
        goto cleanup;

    for (i = 0; i < nsecrets; i++) {
        char uuid[VIR_UUID_STRING_BUFLEN];

        if (virSecretGetUUIDString(secrets[i], uuid) < 0 ||
            VIR_STRDUP(tmp[i], uuid) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, tmp);

 cleanup:
    for (i = 0; i < nsecrets; i++)
        virSecretFree(secrets[i]);
    VIR_FREE(secrets);
    return ret;
}


char **
virshCheckpointNameCompleter(vshControl *ctl,
                             const vshCmd *cmd,
                             unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virDomainPtr dom = NULL;
    virDomainCheckpointPtr *checkpoints = NULL;
    int ncheckpoints = 0;
    size_t i = 0;
    char **ret = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if (!(dom = virshCommandOptDomain(ctl, cmd, NULL)))
        return NULL;

    if ((ncheckpoints = virDomainListAllCheckpoints(dom, &checkpoints,
                                                    flags)) < 0)
        goto error;

    if (VIR_ALLOC_N(ret, ncheckpoints + 1) < 0)
        goto error;

    for (i = 0; i < ncheckpoints; i++) {
        const char *name = virDomainCheckpointGetName(checkpoints[i]);

        if (VIR_STRDUP(ret[i], name) < 0)
            goto error;

        virshDomainCheckpointFree(checkpoints[i]);
    }
    VIR_FREE(checkpoints);
    virshDomainFree(dom);

    return ret;

 error:
    for (; i < ncheckpoints; i++)
        virshDomainCheckpointFree(checkpoints[i]);
    VIR_FREE(checkpoints);
    for (i = 0; i < ncheckpoints; i++)
        VIR_FREE(ret[i]);
    VIR_FREE(ret);
    virshDomainFree(dom);
    return NULL;
}

char **
virshSnapshotNameCompleter(vshControl *ctl,
                           const vshCmd *cmd,
                           unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virDomainPtr dom = NULL;
    virDomainSnapshotPtr *snapshots = NULL;
    int rc;
    int nsnapshots = 0;
    size_t i = 0;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if (!(dom = virshCommandOptDomain(ctl, cmd, NULL)))
        return NULL;

    if ((rc = virDomainListAllSnapshots(dom, &snapshots, flags)) < 0)
        goto cleanup;
    nsnapshots = rc;

    if (VIR_ALLOC_N(tmp, nsnapshots + 1) < 0)
        goto cleanup;

    for (i = 0; i < nsnapshots; i++) {
        const char *name = virDomainSnapshotGetName(snapshots[i]);

        if (VIR_STRDUP(tmp[i], name) < 0)
            goto cleanup;
    }

    VIR_STEAL_PTR(ret, tmp);

 cleanup:
    virshDomainFree(dom);
    for (i = 0; i < nsnapshots; i++)
        virshDomainSnapshotFree(snapshots[i]);
    VIR_FREE(snapshots);
    return ret;
}

static char *
virshPagesizeNodeToString(xmlNodePtr node)
{
    VIR_AUTOFREE(char *) pagesize = NULL;
    VIR_AUTOFREE(char *) unit = NULL;
    unsigned long long byteval = 0;
    const char *suffix = NULL;
    double size = 0;
    char *ret;

    pagesize = virXMLPropString(node, "size");
    unit = virXMLPropString(node, "unit");
    if (virStrToLong_ull(pagesize, NULL, 10, &byteval) < 0)
        return NULL;
    if (virScaleInteger(&byteval, unit, 1024, UINT_MAX) < 0)
        return NULL;
    size = vshPrettyCapacity(byteval, &suffix);
    if (virAsprintf(&ret, "%.0f%s", size, suffix) < 0)
        return NULL;
    return ret;
}

char **
virshAllocpagesPagesizeCompleter(vshControl *ctl,
                                 const vshCmd *cmd ATTRIBUTE_UNUSED,
                                 unsigned int flags)
{
    VIR_AUTOPTR(xmlXPathContext) ctxt = NULL;
    virshControlPtr priv = ctl->privData;
    unsigned int npages = 0;
    VIR_AUTOFREE(xmlNodePtr *) pages = NULL;
    VIR_AUTOPTR(xmlDoc) doc = NULL;
    size_t i = 0;
    const char *cellnum = NULL;
    bool cellno = vshCommandOptBool(cmd, "cellno");
    VIR_AUTOFREE(char *) path = NULL;
    VIR_AUTOFREE(char *) cap_xml = NULL;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if (!(cap_xml = virConnectGetCapabilities(priv->conn)))
        return NULL;

    if (!(doc = virXMLParseStringCtxt(cap_xml, _("capabilities"), &ctxt)))
        return NULL;

    if (cellno && vshCommandOptStringQuiet(ctl, cmd, "cellno", &cellnum) > 0) {
        if (virAsprintf(&path,
                        "/capabilities/host/topology/cells/cell[@id=\"%s\"]/pages",
                        cellnum) < 0)
            return NULL;
    } else {
        if (virAsprintf(&path, "/capabilities/host/cpu/pages") < 0)
            return NULL;
    }

    npages = virXPathNodeSet(path, ctxt, &pages);
    if (npages <= 0)
        return NULL;

    if (VIR_ALLOC_N(tmp, npages + 1) < 0)
        return NULL;

    for (i = 0; i < npages; i++) {
        if (!(tmp[i] = virshPagesizeNodeToString(pages[i])))
            return NULL;
    }

    VIR_STEAL_PTR(ret, tmp);
    return ret;
}


char **
virshSecretEventNameCompleter(vshControl *ctl ATTRIBUTE_UNUSED,
                              const vshCmd *cmd ATTRIBUTE_UNUSED,
                              unsigned int flags)
{
    size_t i;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (VIR_ALLOC_N(tmp, VIR_SECRET_EVENT_ID_LAST + 1) < 0)
        return NULL;

    for (i = 0; i < VIR_SECRET_EVENT_ID_LAST; i++) {
        if (VIR_STRDUP(tmp[i], virshSecretEventCallbacks[i].name) < 0)
            return NULL;
    }

    VIR_STEAL_PTR(ret, tmp);
    return ret;
}


char **
virshNodeDeviceEventNameCompleter(vshControl *ctl ATTRIBUTE_UNUSED,
                                  const vshCmd *cmd ATTRIBUTE_UNUSED,
                                  unsigned int flags)
{
    size_t i = 0;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (VIR_ALLOC_N(tmp, VIR_NODE_DEVICE_EVENT_ID_LAST + 1) < 0)
        return NULL;

    for (i = 0; i < VIR_NODE_DEVICE_EVENT_ID_LAST; i++) {
        if (VIR_STRDUP(tmp[i], virshNodeDeviceEventCallbacks[i].name) < 0)
            return NULL;
    }

    VIR_STEAL_PTR(ret, tmp);
    return ret;
}


char **
virshNodeDeviceCapabilityNameCompleter(vshControl *ctl,
                                       const vshCmd *cmd,
                                       unsigned int flags)
{
    VIR_AUTOSTRINGLIST tmp = NULL;
    const char *cap_str = NULL;
    size_t i = 0;

    virCheckFlags(0, NULL);

    if (vshCommandOptStringQuiet(ctl, cmd, "cap", &cap_str) < 0)
        return NULL;

    if (VIR_ALLOC_N(tmp, VIR_NODE_DEV_CAP_LAST + 1) < 0)
        return NULL;

    for (i = 0; i < VIR_NODE_DEV_CAP_LAST; i++) {
        if (VIR_STRDUP(tmp[i], virNodeDevCapTypeToString(i)) < 0)
            return NULL;
    }

    return virshCommaStringListComplete(cap_str, (const char **)tmp);
}


char **
virshCellnoCompleter(vshControl *ctl,
                     const vshCmd *cmd ATTRIBUTE_UNUSED,
                     unsigned int flags)
{
    VIR_AUTOPTR(xmlXPathContext) ctxt = NULL;
    virshControlPtr priv = ctl->privData;
    unsigned int ncells = 0;
    VIR_AUTOFREE(xmlNodePtr *) cells = NULL;
    VIR_AUTOPTR(xmlDoc) doc = NULL;
    size_t i = 0;
    VIR_AUTOFREE(char *) cap_xml = NULL;
    char **ret = NULL;
    VIR_AUTOSTRINGLIST tmp = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if (!(cap_xml = virConnectGetCapabilities(priv->conn)))
        return NULL;

    if (!(doc = virXMLParseStringCtxt(cap_xml, _("capabilities"), &ctxt)))
        return NULL;

    ncells = virXPathNodeSet("/capabilities/host/topology/cells/cell", ctxt, &cells);
    if (ncells <= 0)
        return NULL;

    if (VIR_ALLOC_N(tmp, ncells + 1))
        return NULL;

    for (i = 0; i < ncells; i++) {
        if (!(tmp[i] = virXMLPropString(cells[i], "id")))
            return NULL;
    }

    VIR_STEAL_PTR(ret, tmp);
    return ret;
}
