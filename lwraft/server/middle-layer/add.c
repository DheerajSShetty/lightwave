/*
 * Copyright © 2012-2015 VMware, Inc.  All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the “License”); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an “AS IS” BASIS, without
 * warranties or conditions of any kind, EITHER EXPRESS OR IMPLIED.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */



#include "includes.h"

static
int
_VmDirEntryDupAttrValueCheck(
    PVDIR_ENTRY  pEntry,
    PSTR*   ppszDupAttributeName
    );

int
VmDirMLAdd(
    PVDIR_OPERATION pOperation
    )
{
    DWORD   dwError = 0;
    PSTR    pszLocalErrMsg = NULL;

    assert(pOperation->conn);

    // establish entry schema context association
    pOperation->request.addReq.pEntry->pSchemaCtx = VmDirSchemaCtxClone(pOperation->pSchemaCtx);
    assert(pOperation->request.addReq.pEntry->pSchemaCtx);

    pOperation->pBECtx->pBE = VmDirBackendSelect(pOperation->reqDn.lberbv.bv_val);
    assert(pOperation->pBECtx->pBE);

    // AnonymousBind Or in case of a failed bind, do not grant add access
    if (pOperation->conn->bIsAnonymousBind || VmDirIsFailedAccessInfo(&pOperation->conn->AccessInfo))
    {
        dwError = LDAP_INSUFFICIENT_ACCESS;
        BAIL_ON_VMDIR_ERROR_WITH_MSG(
                dwError, pszLocalErrMsg,
                "Not bind/authenticate yet");
    }

    if (VmDirRaftDisallowUpdates("Add"))
    {
        dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirInternalAddEntry(pOperation);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszLocalErrMsg);
    return pOperation->ldapResult.errCode;

error:
    VMDIR_SET_LDAP_RESULT_ERROR(&(pOperation->ldapResult), dwError, pszLocalErrMsg);
    goto cleanup;
}

/* VmDirInternalAddEntry: Interface that can be used "internally" by the server code, e.g. to create schema, indices,
 * config etc. entries in the BDB store. One of the main differences between this function and MLAdd is that
 * this function does not send back an LDAP result to the client.
 *
 * Return: VmDir level error code.  Also, pOperation->ldapResult content is set.
 *
 */

int
VmDirInternalAddEntry(
    PVDIR_OPERATION    pOperation
    )
{
    int         retVal = LDAP_SUCCESS;
    BOOLEAN     bHasTxn = FALSE;
    PSTR        pszLocalErrMsg = NULL;
    PVDIR_ENTRY pEntry = pOperation->request.addReq.pEntry;
    PVDIR_OPERATION_ML_METRIC   pMLMetrics = NULL;
    extern DWORD    VmDirAddRaftPreCommit(PVDIR_ENTRY pEntry, PVDIR_OPERATION pAddOp);

    assert(pOperation && pOperation->pBECtx->pBE);

    pMLMetrics = &pOperation->MLMetrics;
    VMDIR_COLLECT_TIME(pMLMetrics->iMLStartTime);

    if (VmDirdState() == VMDIRD_STATE_READ_ONLY)
    {
        retVal = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        BAIL_ON_VMDIR_ERROR_WITH_MSG(
                retVal, pszLocalErrMsg,
                "Server in read-only mode");
    }

    // make sure we have minimum DN length
    if (pEntry->dn.lberbv_len < 3)
    {
        retVal = VMDIR_ERROR_INVALID_REQUEST;
        BAIL_ON_VMDIR_ERROR_WITH_MSG(
                retVal, pszLocalErrMsg,
                "Invalid DN length - (%u)",
                pEntry->dn.lberbv_len);
    }

    // Make sure Attribute has its ATDesc set
    retVal = VmDirSchemaCheckSetAttrDesc(pEntry->pSchemaCtx, pEntry);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "%s",
            VDIR_SAFE_STRING(VmDirSchemaCtxGetErrorMsg(pEntry->pSchemaCtx)));

    // Normalize DN
    retVal = VmDirNormalizeDN(&(pEntry->dn), pEntry->pSchemaCtx);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "DN normalization failed - (%u)(%s)",
            retVal,
            VDIR_SAFE_STRING(VmDirSchemaCtxGetErrorMsg(pEntry->pSchemaCtx)));

    // Acquire schema modification mutex
    retVal = VmDirSchemaModMutexAcquire(pOperation);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "Failed to lock schema mod mutex (%u)",
            retVal);

    // Parse Parent DN
    retVal = VmDirGetParentDN(&pEntry->dn, &pEntry->pdn);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "Get ParentDn failed - (%u)",
            retVal);

    VMDIR_COLLECT_TIME(pMLMetrics->iPrePluginsStartTime);

    retVal = VmDirExecutePreAddPlugins(pOperation, pEntry, retVal);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "PreAdd plugin failed - (%u)",
            retVal);

    VMDIR_COLLECT_TIME(pMLMetrics->iPrePlugunsEndTim);

    if (pOperation->opType != VDIR_OPERATION_TYPE_REPL)
    {
        // Entry schema check
        retVal = VmDirSchemaCheck(pEntry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG(
                retVal, pszLocalErrMsg,
                "schema error - (%u)(%s)",
                retVal,
                VDIR_SAFE_STRING(VmDirSchemaCtxGetErrorMsg(pEntry->pSchemaCtx)));
    }

    // Normalize all attribute value
    retVal = VmDirEntryAttrValueNormalize(pEntry, FALSE /*all attributes*/);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "Attr value normalization failed - (%u)",
            retVal);

    {
        PSTR pszDupAttributeName = NULL;

        // Make sure we have no duplicate attributes
        retVal = _VmDirEntryDupAttrValueCheck(pEntry, &pszDupAttributeName);
        BAIL_ON_VMDIR_ERROR_WITH_MSG(
                retVal, pszLocalErrMsg,
                "Invalid or duplicate (%s)",
                VDIR_SAFE_STRING(pszDupAttributeName));
    }

    VMDIR_COLLECT_TIME(pMLMetrics->iBETxnBeginStartTime);

    retVal = pOperation->pBEIF->pfnBETxnBegin(pOperation->pBECtx, VDIR_BACKEND_TXN_WRITE);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "txn begin (%u)(%s)",
            retVal,
            VDIR_SAFE_STRING(pOperation->pBEErrorMsg));

    VMDIR_COLLECT_TIME(pMLMetrics->iBETxnBeginEndTime);
    bHasTxn = TRUE;

    // get parent entry
    if (pEntry->pdn.lberbv.bv_val)
    {
        PVDIR_ENTRY pParentEntry = NULL;

        retVal = VmDirAllocateMemory(sizeof(*pEntry), (PVOID)&pParentEntry);
        BAIL_ON_VMDIR_ERROR(retVal);

        retVal = pOperation->pBEIF->pfnBEDNToEntry(
                pOperation->pBECtx,
                pOperation->pSchemaCtx,
                &pEntry->pdn,
                pParentEntry,
                VDIR_BACKEND_ENTRY_LOCK_READ);
        if (retVal)
        {
            VmDirFreeEntryContent(pParentEntry);
            VMDIR_SAFE_FREE_MEMORY(pParentEntry);

            switch (retVal)
            {
            case VMDIR_ERROR_BACKEND_ENTRY_NOTFOUND:
                BAIL_ON_VMDIR_ERROR_WITH_MSG(
                        retVal, pszLocalErrMsg,
                        "parent (%s) not found, (%s)",
                        pEntry->pdn.lberbv_val,
                        VDIR_SAFE_STRING(pOperation->pBEErrorMsg));

            default:
                BAIL_ON_VMDIR_ERROR_WITH_MSG(
                        retVal, pszLocalErrMsg,
                        "parent (%s) lookup failed, (%s)",
                        pEntry->pdn.lberbv_val,
                        VDIR_SAFE_STRING(pOperation->pBEErrorMsg));
            }
        }

        pEntry->pParentEntry = pParentEntry;    // pEntry takes over pParentEntry
        pParentEntry = NULL;
    }

    // enforce schema structure rule
    retVal = VmDirEntryCheckStructureRule(pOperation, pEntry);
    BAIL_ON_VMDIR_ERROR(retVal);

    // only when there is parent Entry, ACL check is done
    if (pEntry->pParentEntry)
    {
        retVal = VmDirSrvAccessCheck(
                pOperation,
                &pOperation->conn->AccessInfo,
                pEntry->pParentEntry,
                VMDIR_RIGHT_DS_CREATE_CHILD);
        BAIL_ON_VMDIR_ERROR(retVal);
    }

    if (pOperation->opType != VDIR_OPERATION_TYPE_REPL)
    {
        // Skip SD in case of a replication operation (SD should exist by then anyways)
        // so that we do not manipulate data in replication operation (replicate 'purely')
        retVal = VmDirComputeObjectSecurityDescriptor(
                &pOperation->conn->AccessInfo, pEntry, pEntry->pParentEntry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG(
                retVal, pszLocalErrMsg,
                "Prepare object SD failed, (%u)",
                retVal);

        // check and read lock dn referenced entries
        retVal = pOperation->pBEIF->pfnBEChkDNReference(pOperation->pBECtx, pEntry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG(
                retVal, pszLocalErrMsg,
                "BEChkDNRef (%u)(%s)",
                retVal,
                VDIR_SAFE_STRING(pOperation->pBEErrorMsg));
    }

    // add vmwRaftLogChanged attribute
    retVal = VmDirUpdateRaftLogChangedAttr(pOperation, pEntry);
    BAIL_ON_VMDIR_ERROR(retVal);

    retVal = pOperation->pBEIF->pfnBEEntryAdd(pOperation->pBECtx, pEntry);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "BEEntryAdd (%u)(%s)",
            retVal,
            VDIR_SAFE_STRING(pOperation->pBEErrorMsg));

    retVal = VmDirAddRaftPreCommit(pEntry, pOperation);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "VmDirAddRaftPreCommit error (%u)",
            retVal);

    VMDIR_COLLECT_TIME(pMLMetrics->iBETxnCommitStartTime);

    retVal = pOperation->pBEIF->pfnBETxnCommit(pOperation->pBECtx);
    BAIL_ON_VMDIR_ERROR_WITH_MSG(
            retVal, pszLocalErrMsg,
            "txn commit (%u)(%s)",
            retVal,
            VDIR_SAFE_STRING(pOperation->pBEErrorMsg));

    VMDIR_COLLECT_TIME(pMLMetrics->iBETxnCommitEndTime);
    bHasTxn = FALSE;

    if (!pOperation->bSuppressLogInfo)
    {
        VMDIR_LOG_INFO(
                VMDIR_LOG_MASK_ALL,
                "Add Entry (%s)",
                VDIR_SAFE_STRING(pEntry->dn.lberbv_val));
    }

cleanup:
    {
        int iPostCommitPluginRtn = 0;

        VMDIR_COLLECT_TIME(pMLMetrics->iPostPluginsStartTime);

        // Execute post Add commit plugin logic
        iPostCommitPluginRtn = VmDirExecutePostAddCommitPlugins(pOperation, pEntry, retVal);
        if (iPostCommitPluginRtn != LDAP_SUCCESS &&
            iPostCommitPluginRtn != pOperation->ldapResult.errCode) // pass through
        {
            VMDIR_LOG_ERROR(
                    LDAP_DEBUG_ANY,
                    "InternalAddEntry: VdirExecutePostAddCommitPlugins %s - code(%d)",
                    pEntry->dn.lberbv_val,
                    iPostCommitPluginRtn);
        }

        VMDIR_COLLECT_TIME(pMLMetrics->iPostPlugunsEndTime);
    }

    // Release schema modification mutex
    (VOID)VmDirSchemaModMutexRelease(pOperation);

    // collect metrics
    VMDIR_COLLECT_TIME(pMLMetrics->iMLEndTime);
    VmDirInternalMetricsUpdate(pOperation);

    VMDIR_SAFE_FREE_MEMORY(pszLocalErrMsg);
    return retVal;

error:

    if (bHasTxn)
    {
        VMDIR_COLLECT_TIME(pMLMetrics->iBETxnCommitStartTime);
        pOperation->pBEIF->pfnBETxnAbort(pOperation->pBECtx);
        VMDIR_COLLECT_TIME(pMLMetrics->iBETxnCommitEndTime);
    }

    VMDIR_SET_LDAP_RESULT_ERROR(&pOperation->ldapResult, retVal, pszLocalErrMsg);
    goto cleanup;
}

/*
 * Enforce DIT Structure rules
 * This function is called within InternalAdd/ModifyEntry transaction loop, so it should
 * have no side effect in terms of transaction abort/retry.
 *
 * RETURN:
 * LDAP_OPERATIONS_ERROR    - all other errors
 *
 */
int
VmDirEntryCheckStructureRule(
    PVDIR_OPERATION  pOperation,
    PVDIR_ENTRY      pEntry
    )
{
    int     retVal = 0;
    PSTR    pszLocalErrMsg = NULL;

    assert(pOperation && pEntry);

    // enforce schema DIT structure rule
    retVal = VmDirSchemaCheckDITStructure(
                    pEntry->pSchemaCtx,
                    pEntry->pParentEntry,
                    pEntry);
    if (retVal)
    {
        VmDirAllocateStringPrintf(&pszLocalErrMsg,
                "DIT Structure rule check failed. (%s)",
                VDIR_SAFE_STRING(VmDirSchemaCtxGetErrorMsg(pEntry->pSchemaCtx)));
        retVal = VMDIR_ERROR_STRUCTURE_VIOLATION;
    }
    BAIL_ON_VMDIR_ERROR(retVal);

cleanup:

    VMDIR_SAFE_FREE_MEMORY(pszLocalErrMsg);

    return retVal;

error:

    VMDIR_SET_LDAP_RESULT_ERROR(&(pOperation->ldapResult), retVal, pszLocalErrMsg);

    goto cleanup;
}

/*
 * Assume pAttr already been normalized and have pATDesc setup.
 */
int
VmDirAttributeDupValueCheck(
    PVDIR_ATTRIBUTE  pAttr,
    PSTR*            ppszDupAttributeName
    )
{
    int     iError = 0;

    assert(pAttr && ppszDupAttributeName);

    if (pAttr->pATDesc->bSingleValue == FALSE && pAttr->numVals > 1)
    {   // TODO, nested loop may be ok for most cases. however, if attr.numVals
        // becomes big, we should use better algorithm instead.
        unsigned int     iInnerLoop = 0;
        unsigned int     iOuterLoop = 0;

        for (iOuterLoop = 0; iOuterLoop < pAttr->numVals - 1; iOuterLoop++)
        {
            PSTR        nVal = BERVAL_NORM_VAL(pAttr->vals[iOuterLoop]);
            ber_len_t   nValLen = BERVAL_NORM_LEN(pAttr->vals[iOuterLoop]);

            for (iInnerLoop = iOuterLoop + 1; iInnerLoop < pAttr->numVals; iInnerLoop++)
            {
                PSTR        nNextVal = BERVAL_NORM_VAL(pAttr->vals[iInnerLoop]);
                ber_len_t   nNextValLen = BERVAL_NORM_LEN(pAttr->vals[iInnerLoop]);

                if (nValLen == nNextValLen && memcmp(nVal, nNextVal, nValLen) == 0)
                {
                    *ppszDupAttributeName = pAttr->pATDesc->pszName;
                    iError = VMDIR_ERROR_TYPE_OR_VALUE_EXISTS;
                    BAIL_ON_VMDIR_ERROR(iError);
                }

            }
        }
    }

error:

    return iError;
}

/*
 * Assume pEntry attributes already been normalized and have pATDesc setup.
 */
static
int
_VmDirEntryDupAttrValueCheck(
    PVDIR_ENTRY  pEntry,
    PSTR*        ppszDupAttributeName
    )
{
    int         iError = 0;
    PVDIR_ATTRIBUTE  pAttr = NULL;

    assert(pEntry && ppszDupAttributeName);

    for (pAttr = pEntry->attrs; pAttr; pAttr = pAttr->next)
    {
        iError = VmDirAttributeDupValueCheck(pAttr, ppszDupAttributeName);
        BAIL_ON_VMDIR_ERROR(iError);
    }


error:

    return iError;
}

int
VmDirEntryAttrValueNormalize(
    PVDIR_ENTRY     pEntry,
    BOOLEAN         bIndexAttributeOnly
    )
{
    DWORD           dwError = 0;
    PVDIR_ATTRIBUTE pAttr = NULL;
    PVDIR_INDEX_CFG pIndexCfg = NULL;

    assert(pEntry && pEntry->pSchemaCtx);

    for (pAttr = pEntry->attrs; pAttr; pAttr = pAttr->next)
    {
        if (bIndexAttributeOnly == TRUE)
        {
            dwError = VmDirIndexCfgAcquire(
                    pAttr->type.lberbv.bv_val, VDIR_INDEX_WRITE, &pIndexCfg);
            BAIL_ON_VMDIR_ERROR(dwError);
        }

        if (bIndexAttributeOnly == FALSE || pIndexCfg)
        {
            unsigned int    iCnt = 0;

            if (pAttr->pATDesc == NULL)
            {
                if ((pAttr->pATDesc = VmDirSchemaAttrNameToDesc(
                        pEntry->pSchemaCtx, pAttr->type.lberbv.bv_val)) == NULL)
                {
                    dwError = VMDIR_ERROR_NO_SUCH_ATTRIBUTE;
                    BAIL_ON_VMDIR_ERROR(dwError);
                }
            }

            for (iCnt=0; iCnt < pAttr->numVals; iCnt++)
            {
                dwError = VmDirSchemaBervalNormalize(
                        pEntry->pSchemaCtx, pAttr->pATDesc, &pAttr->vals[iCnt]);
                BAIL_ON_VMDIR_ERROR(dwError);
            }
        }
        VmDirIndexCfgRelease(pIndexCfg);
        pIndexCfg = NULL;
    }

error:
    VmDirIndexCfgRelease(pIndexCfg);
    return dwError;
}
