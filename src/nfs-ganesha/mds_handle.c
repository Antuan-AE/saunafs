/*
   Copyright 2017 Skytechnology sp. z o.o.

   This file is part of LizardFS.

   LizardFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LizardFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LizardFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "pnfs_utils.h"

#include "context_wrap.h"
#include "lzfs_fsal_methods.h"
#include "protocol/MFSCommunication.h"

/**
 * @brief Grant a layout segment.
 *
 * pNFS functions.
 *
 * This function is called by nfs41_op_layoutget. It may be called multiple times,
 * to satisfy a request with multiple segments. The FSAL may track state (what
 * portion of the request has been or remains to be satisfied or any other
 * information it wishes) in the bookkeeper member of res. Each segment may have
 * FSAL-specific information associated with it its segid. This segid will be
 * supplied to the FSAL when the segment is committed or returned.
 *
 * When the granting the last segment it intends to grant, the FSAL must set the
 * last_segment flag in res.
 *
 * @param[in] objectHandle      The handle of the file on which the layout is requested
 * @param[out] xdrStream        An XDR stream to which the FSAL must encode the layout
 *                              specific portion of the granted layout segment
 * @param[in] arguments         Input arguments of the function
 * @param[in,out] output        In/out and output arguments of the function
 *
 * @returns: Valid error codes in RFC 5661, pp. 366-7.
 */
static nfsstat4 _layoutget(struct fsal_obj_handle *objectHandle,
                           XDR *xdrStream,
                           const struct fsal_layoutget_arg *arguments,
                           struct fsal_layoutget_res *output) {
	struct FSHandle *handle;
	struct DataServerWire dataServerWire;

	struct gsh_buffdesc dsBufferDescriptor = {
		.addr = &dataServerWire,
		.len = sizeof(struct DataServerWire)
	};

	struct pnfs_deviceid deviceid = DEVICE_ID_INIT_ZERO(FSAL_ID_LIZARDFS);
	nfl_util4 layout_util = 0;
	nfsstat4 nfs_status = NFS4_OK;

	handle = container_of(objectHandle, struct FSHandle, fileHandle);

	if (arguments->type != LAYOUT4_NFSV4_1_FILES) {
		LogMajor(COMPONENT_PNFS, "Unsupported layout type: %x",
		         arguments->type);

		return NFS4ERR_UNKNOWN_LAYOUTTYPE;
	}

	LogDebug(COMPONENT_PNFS, "will issue layout offset: %" PRIu64
	         " length: %" PRIu64, output->segment.offset,
	         output->segment.length);

	deviceid.device_id2 = handle->export->export.export_id;
	deviceid.devid = handle->inode;

	dataServerWire.inode = handle->inode;
	layout_util = MFSCHUNKSIZE;

	nfs_status = FSAL_encode_file_layout(xdrStream, &deviceid, layout_util,
	                                     0, 0, &op_ctx->ctx_export->export_id,
	                                     1, &dsBufferDescriptor);

	if (nfs_status) {
		LogMajor(COMPONENT_PNFS, "Failed to encode nfsv4_1_file_layout.");
		return nfs_status;
	}

	output->return_on_close = true;
	output->last_segment = true;

	return nfs_status;
}


/**
 * @brief Potentially return one layout segment.
 *
 * This function is called once on each segment matching the IO mode and
 * intersecting the range specified in a LAYOUTRETURN operation or for all
 * layouts corresponding to a given stateid on last close, lease expiry, or
 * a layoutreturn with a return-type of FSID or ALL. Whether it is called in
 * the former or latter case is indicated by the synthetic flag in the arg
 * structure, with synthetic being true in the case of last-close or lease expiry.
 *
 * If arg->dispose is true, all resources associated with the layout must be freed.
 *
 * @param[in] objectHandle      The object on which a segment is to be returned
 * @param[in] xdrStream         In the case of a non-synthetic return, this is an
 *                              XDR stream corresponding to the layout type-specific
 *                              argument to LAYOUTRETURN. In the case of a synthetic
 *                              or bulk return, this is a NULL pointer
 * @param[in] arguments         Input arguments of the function
 *
 * @returns: Valid error codes in RFC 5661, p. 367.
 */
static nfsstat4 _layoutreturn(struct fsal_obj_handle *objectHandle,
                              XDR *xdrStream,
                              const struct fsal_layoutreturn_arg *arguments) {
	(void) objectHandle;
	(void) xdrStream;

	if (arguments->lo_type != LAYOUT4_NFSV4_1_FILES) {
		LogDebug(COMPONENT_PNFS, "Unsupported layout type: %x",
		         arguments->lo_type);

		return NFS4ERR_UNKNOWN_LAYOUTTYPE;
	}

	return NFS4_OK;
}

/**
 * @brief Function to know if the client set a new offset
 *
 * @param[in] arguments          Input parameters to FSAL layoutcommit
 * @param[in] previousReply      Attributes returned by getattr()
 *
 * @retval true : If the client set a new offset
 * @retval false: Otherwise
 */
bool isOffsetChangedByClient(const struct fsal_layoutcommit_arg *arguments,
                             struct liz_attr_reply previousReply) {
	return arguments->new_offset &&
	       previousReply.attr.st_size < (long)arguments->last_write + 1;
}

/**
 * @brief Function to know if the client provided a new value for mtime
 *
 * @param[in] arguments          Input parameters to FSAL layoutcommit
 * @param[in] previousReply      Attributes returned by getattr()
 *
 * @retval true : If the client provided a new value for mtime
 * @retval false: Otherwise
 */
bool hasRecentModificationTime(const struct fsal_layoutcommit_arg *arguments,
                               struct liz_attr_reply previousReply) {
	return arguments->time_changed &&
	       (arguments->new_time.seconds  > previousReply.attr.st_mtim.tv_sec ||
	       (arguments->new_time.seconds == previousReply.attr.st_mtim.tv_sec &&
	        arguments->new_time.nseconds > previousReply.attr.st_mtim.tv_nsec));
}

/**
 * @brief Commit a segment of a layout.
 *
 * This function is called once on every segment of a layout.
 * The FSAL may avoid being called again after it has finished all tasks
 * necessary for the commit by setting res->commit_done to true.
 *
 * The calling function does not inspect or act on the value of size_supplied
 * or new_size until after the last call to FSAL_layoutcommit.
 *
 * @param[in] objectHandle      The object on which to commit
 * @param[in] xdrStream         An XDR stream containing the layout type-specific
 *                              portion of the LAYOUTCOMMIT arguments
 * @param[in] arguments         Input arguments of the function
 * @param[in,out] output        In/out and output arguments of the function
 *
 * @returns: Valid error codes in RFC 5661, p. 366.
 */
static nfsstat4 _layoutcommit(struct fsal_obj_handle *objectHandle,
                              XDR *xdrStream,
                              const struct fsal_layoutcommit_arg *arguments,
                              struct fsal_layoutcommit_res *output) {
	(void) xdrStream;

	struct FSExport *export;
	struct FSHandle *handle;
	struct liz_attr_reply previousReply;

	// FIXME(haze): Does this function make sense for our implementation ?

	if (arguments->type != LAYOUT4_NFSV4_1_FILES) {
		LogCrit(COMPONENT_PNFS, "Unsupported layout type: %x", arguments->type);
		return NFS4ERR_UNKNOWN_LAYOUTTYPE;
	}

	export = container_of(op_ctx->fsal_export, struct FSExport, export);
	handle = container_of(objectHandle, struct FSHandle, fileHandle);

	int rc = fs_getattr(export->fsInstance, &op_ctx->creds,
	                    handle->inode, &previousReply);

	if (rc < 0) {
		LogCrit(COMPONENT_PNFS, "Error '%s' in attempt to get "
		        "attributes of file %lli.",
		        liz_error_string(liz_last_err()),
		        (long long)handle->inode);

		return Nfs4LastError();
	}

	struct stat posixAttributes;
	int mask = 0;

	memset(&posixAttributes, 0, sizeof(posixAttributes));

	if (isOffsetChangedByClient(arguments, previousReply)) {
		mask |= LIZ_SET_ATTR_SIZE;
		posixAttributes.st_size = arguments->last_write + 1;
		output->size_supplied = true;
		output->new_size = arguments->last_write + 1;
	}

	if (hasRecentModificationTime(arguments, previousReply)) {
		posixAttributes.st_mtim.tv_sec = arguments->new_time.seconds;
		posixAttributes.st_mtim.tv_sec = arguments->new_time.nseconds;
		mask |= LIZ_SET_ATTR_MTIME;
	}

	liz_attr_reply_t reply;
	rc = fs_setattr(export->fsInstance, &op_ctx->creds, handle->inode,
	                &posixAttributes, mask, &reply);

	if (rc < 0) {
		LogCrit(COMPONENT_PNFS, "Error '%s' in attempt to set attributes "
		        "of file %lli.", liz_error_string(liz_last_err()),
		        (long long)handle->inode);
		return Nfs4LastError();
	}

	output->commit_done = true;
	return NFS4_OK;
}

/**
 * @brief Initialize pNFS related operations
 *
 * @param[in] ops      FSAL object operations vector
 *
 * @returns: Nothing. After running this method, the operations vector
 *           is initialized with the pNFS related operations.
 */
void initializeMetaDataServerOperations(struct fsal_obj_ops *ops) {
	ops->layoutget = _layoutget;
	ops->layoutreturn = _layoutreturn;
	ops->layoutcommit = _layoutcommit;
}
