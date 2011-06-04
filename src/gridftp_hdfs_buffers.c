
#include "gridftp_hdfs.h"

/*************************************************************************
 *  use_file_buffer
 *  ---------------
 *  Decide whether we should use a file buffer based on the current
 *  memory usage.
 *  Returns 1 if we should use a file buffer.
 *  Else, returns 0.
 ************************************************************************/
int use_file_buffer(globus_l_gfs_hdfs_handle_t * hdfs_handle) {
        int buffer_count = hdfs_handle->buffer_count;
 
		  if (buffer_count >= hdfs_handle->max_buffer_count-1) {
            return 1;
		  }
        if ((hdfs_handle->using_file_buffer == 1) && (buffer_count > hdfs_handle->max_buffer_count/2))
            return 1;
        return 0;
}

/*************************************************************************
 *  remove_file_buffer
 *  ------------------
 *  This is called when cleaning up a file buffer. The file on disk is removed and
 *  the internal memory for storing the filename is freed.
 ************************************************************************/
void
remove_file_buffer(globus_l_gfs_hdfs_handle_t * hdfs_handle) {
    if (hdfs_handle->tmp_file_pattern) {
	snprintf(err_msg, MSG_SIZE, "Removing file buffer %s.\n", hdfs_handle->tmp_file_pattern);
	globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
        unlink(hdfs_handle->tmp_file_pattern);
        globus_free(hdfs_handle->tmp_file_pattern);
	hdfs_handle->tmp_file_pattern = (char *)NULL;
    }
}

/**
 *  Store the current output to a buffer.
 */
globus_result_t hdfs_store_buffer(globus_l_gfs_hdfs_handle_t * hdfs_handle, globus_byte_t* buffer, globus_off_t offset, globus_size_t nbytes) {
		  GlobusGFSName(globus_l_gfs_hdfs_store_buffer);
		  globus_result_t rc = GLOBUS_SUCCESS;
		  int i, cnt = hdfs_handle->buffer_count;
		  short wrote_something = 0;
		  if (hdfs_handle == NULL) {
					 rc = GlobusGFSErrorGeneric("Storing buffer for un-allocated transfer");
					 return rc;
		  }

        // Determine the type of buffer to use; allocate or transfer buffers as necessary
        int use_buffer = use_file_buffer(hdfs_handle);
        if ((use_buffer == 1) && (hdfs_handle->using_file_buffer == 0)) {
            // Turn on file buffering, copy data from the current memory buffer.
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Switching from memory buffer to file buffer.\n");

            char *tmpdir=getenv("TMPDIR");
            if (tmpdir == NULL) {
                tmpdir = "/tmp";
            }
            hdfs_handle->tmp_file_pattern = globus_malloc(sizeof(char) * (strlen(tmpdir) + 32));
            sprintf(hdfs_handle->tmp_file_pattern, "%s/gridftp-hdfs-buffer-XXXXXX", tmpdir);

            hdfs_handle->tmpfilefd = mkstemp(hdfs_handle->tmp_file_pattern);
            int filedes = hdfs_handle->tmpfilefd;
            if (filedes == -1) {
                globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Failed to determine file descriptor of temporary file.\n");
                rc = GlobusGFSErrorGeneric("Failed to determine file descriptor of temporary file.");
                return rc;
            }
            snprintf(err_msg, MSG_SIZE, "Created file buffer %s.\n", hdfs_handle->tmp_file_pattern);
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
            char * tmp_write = globus_calloc(hdfs_handle->block_size, sizeof(globus_byte_t));
            if (tmp_write == NULL) {
                globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Could not allocate memory for dumping file buffer.\n");
            }
            /* Write into the file to create its initial size */
            for (i=0; i<cnt; i++) {
                if (write(filedes, tmp_write, sizeof(globus_byte_t)*hdfs_handle->block_size) < 0) {
                    globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Failed to initialize backing file.\n");
                    rc = GlobusGFSErrorGeneric("Failed to initialize backing file.");
                    return rc;
                }
            }
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Pre-filled file buffer with empty data.\n");
            globus_free(tmp_write);
            globus_byte_t * file_buffer = mmap(0, hdfs_handle->block_size*hdfs_handle->max_file_buffer_count*sizeof(globus_byte_t), PROT_READ | PROT_WRITE, MAP_SHARED, filedes, 0);
            if (file_buffer == (globus_byte_t *)-1) {
                if (errno == ENOMEM) {
                    snprintf(err_msg, MSG_SIZE, "Error mmapping the file buffer (%ld bytes): errno=ENOMEM\n", hdfs_handle->block_size*hdfs_handle->max_file_buffer_count*sizeof(globus_byte_t));
                    globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, err_msg);
                } else {
                    snprintf(err_msg, MSG_SIZE, "Error mmapping the file buffer: errno=%d\n", errno);
                    globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, err_msg);
                }
                /*
                 * Regardless of the error, remove the file buffer.
                 */
                remove_file_buffer(hdfs_handle);
                /*
                 * Is this the proper way to exit from here?
                 */
                rc = GlobusGFSErrorGeneric("Failed to mmap() the file buffer.");
                return rc;
            }
            memcpy(file_buffer, hdfs_handle->buffer, cnt*hdfs_handle->block_size*sizeof(globus_byte_t));
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Memory buffers copied to disk buffer.\n");
            globus_free(hdfs_handle->buffer);
            hdfs_handle->buffer = file_buffer;
            hdfs_handle->using_file_buffer = 1;
        } else if (use_buffer == 1) {
            // Do nothing.  Continue to use the file buffer for now.
        } else if (hdfs_handle->using_file_buffer == 1 && cnt < hdfs_handle->max_buffer_count) {
            // Turn off file buffering; copy data to a new memory buffer
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Switching from file buffer to memory buffer.\n");
            globus_byte_t * tmp_buffer = globus_malloc(sizeof(globus_byte_t)*hdfs_handle->block_size*cnt);
            if (tmp_buffer == NULL) {
                rc = GlobusGFSErrorGeneric("Memory allocation error.");
                globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Memory allocation error.");
                return rc;
            }
            memcpy(tmp_buffer, hdfs_handle->buffer, cnt*hdfs_handle->block_size*sizeof(globus_byte_t));
            munmap(hdfs_handle->buffer, hdfs_handle->block_size*hdfs_handle->buffer_count*sizeof(globus_byte_t));
            hdfs_handle->using_file_buffer = 0;
            close(hdfs_handle->tmpfilefd);
	    remove_file_buffer(hdfs_handle);
            hdfs_handle->buffer = tmp_buffer;
        } else {
            // Do nothing.  Continue to use the file buffer for now.
        }

        // Search for a free space in our buffer, and then actually make the copy.
		  for (i = 0; i<cnt; i++) {
					 if (hdfs_handle->used[i] == 0) {
								snprintf(err_msg, MSG_SIZE, "Stored some bytes in buffer %d; offset %lu.\n", i, offset);
								globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, err_msg);
            hdfs_handle->nbytes[i] = nbytes;
            hdfs_handle->offsets[i] = offset;
            hdfs_handle->used[i] = 1;
            wrote_something=1;
            memcpy(hdfs_handle->buffer+i*hdfs_handle->block_size, buffer, nbytes*sizeof(globus_byte_t));
            break;
        }
    }

    // Check to see how many unused buffers we have;
    i = cnt;
    while (i>0) {
        i--;
        if (hdfs_handle->used[i] == 1) {
            break;
        }
    }
    i++;
    snprintf(err_msg, MSG_SIZE, "There are %i extra buffers.\n", cnt-i);
    globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, err_msg);
    // If there are more than 10 unused buffers, deallocate.
    if (cnt - i > 10) {
        snprintf(err_msg, MSG_SIZE, "About to deallocate %i buffers; %i will be left.\n", cnt-i, i);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
        hdfs_handle->buffer_count = i;
        hdfs_handle->nbytes = globus_realloc(hdfs_handle->nbytes, hdfs_handle->buffer_count*sizeof(globus_size_t));
        hdfs_handle->offsets = globus_realloc(hdfs_handle->offsets, hdfs_handle->buffer_count*sizeof(globus_off_t));
        hdfs_handle->used = globus_realloc(hdfs_handle->used, hdfs_handle->buffer_count*sizeof(short));
        if (hdfs_handle->using_file_buffer == 0)
            hdfs_handle->buffer = globus_realloc(hdfs_handle->buffer, hdfs_handle->buffer_count*hdfs_handle->block_size*sizeof(globus_byte_t));
        else {
            // Truncate the file holding our backing data (note we don't resize the mmap).
            if (ftruncate(hdfs_handle->tmpfilefd, hdfs_handle->buffer_count*hdfs_handle->block_size*sizeof(globus_byte_t))) {
                rc = GlobusGFSErrorGeneric("Unable to truncate our file-backed data.");
                globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Unable to truncate our file-backed data.\n");
            }
            lseek(hdfs_handle->tmpfilefd, 0, SEEK_END);
        }
        if (hdfs_handle->buffer == NULL || hdfs_handle->nbytes==NULL || hdfs_handle->offsets==NULL || hdfs_handle->used==NULL) {
            rc = GlobusGFSErrorGeneric("Memory allocation error.");
            globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Memory allocation error.");
            globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            return rc;
        }
    }

    // If wrote_something=0, then we have filled up all our buffers; allocate a new one.
    if (wrote_something == 0) {
        hdfs_handle->buffer_count += 1;
        snprintf(err_msg, MSG_SIZE, "Initializing buffer number %d.\n", hdfs_handle->buffer_count);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
        // Refuse to allocate more than the max.
        if ((hdfs_handle->using_file_buffer == 0) && (hdfs_handle->buffer_count == hdfs_handle->max_buffer_count)) {
            // Out of memory buffers; we really shouldn't hit this code anymore.
            char * hostname = globus_malloc(sizeof(char)*256);
            memset(hostname, '\0', sizeof(char)*256);
            if (gethostname(hostname, 255) != 0) {
                sprintf(hostname, "UNKNOWN");
            }
            snprintf(err_msg, MSG_SIZE, "Allocated all %i memory buffers on server %s; aborting transfer.", hdfs_handle->max_buffer_count, hostname);
            globus_free(hostname);
            rc = GlobusGFSErrorGeneric(err_msg);
            globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Failed to store data into HDFS buffer.\n");
        } else if ((hdfs_handle->using_file_buffer == 1) && (hdfs_handle->buffer_count == hdfs_handle->max_file_buffer_count)) {
            // Out of file buffers.
            char * hostname = globus_malloc(sizeof(char)*256);
            memset(hostname, '\0', sizeof(char)*256);
            if (gethostname(hostname, 255) != 0) {
                sprintf(hostname, "UNKNOWN");
            }
            snprintf(err_msg, MSG_SIZE, "Allocated all %i file-backed buffers on server %s; aborting transfer.", hdfs_handle->max_file_buffer_count, hostname);
            globus_free(hostname);
            rc = GlobusGFSErrorGeneric(err_msg);
            globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Failed to store data into HDFS buffer.\n");
        } else {
            // Increase the size of all our buffers which track memory usage
            hdfs_handle->nbytes = globus_realloc(hdfs_handle->nbytes, hdfs_handle->buffer_count*sizeof(globus_size_t));
            hdfs_handle->offsets = globus_realloc(hdfs_handle->offsets, hdfs_handle->buffer_count*sizeof(globus_off_t));
            hdfs_handle->used = globus_realloc(hdfs_handle->used, hdfs_handle->buffer_count*sizeof(short));
            hdfs_handle->used[hdfs_handle->buffer_count-1] = 1;
            // Only reallocate the physical buffer if we're using a memory buffer, otherwise we screw up our mmap
            if (hdfs_handle->using_file_buffer == 0) {
                hdfs_handle->buffer = globus_realloc(hdfs_handle->buffer, hdfs_handle->buffer_count*hdfs_handle->block_size*sizeof(globus_byte_t));
            } else {
                // This not only extends the size of our file, but we extend it with the desired buffer data.
                lseek(hdfs_handle->tmpfilefd, (hdfs_handle->buffer_count-1)*hdfs_handle->block_size, SEEK_SET);
                if (write(hdfs_handle->tmpfilefd, buffer, nbytes*sizeof(globus_byte_t)) < 0) {
                    rc = GlobusGFSErrorGeneric("Unable to extend our file-backed buffers; aborting transfer.");
                    globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Unable to extend our file-backed buffers; aborting transfer.\n");
                }
                // If our buffer was too small, 
                if (nbytes < hdfs_handle->block_size) {
                    int addl_size = hdfs_handle->block_size-nbytes;
                    char * tmp_write = globus_calloc(addl_size, sizeof(globus_byte_t));
                    if (write(hdfs_handle->tmpfilefd, tmp_write, sizeof(globus_byte_t)*addl_size) < 0) {
                        rc = GlobusGFSErrorGeneric("Unable to extend our file-backed buffers; aborting transfer.");
                        globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Unable to extend our file-backed buffers; aborting transfer.\n");
                    }
                    globus_free(tmp_write);
                }
                //hdfs_handle->buffer = mmap(hdfs_handle->buffer, hdfs_handle->block_size*hdfs_handle->max_file_buffer_count*sizeof(globus_byte_t), PROT_READ | PROT_WRITE, MAP_PRIVATE, hdfs_handle->tmpfilefd, 0);
            }
            if (hdfs_handle->buffer == NULL || hdfs_handle->nbytes==NULL || hdfs_handle->offsets==NULL || hdfs_handle->used==NULL) {  
                rc = GlobusGFSErrorGeneric("Memory allocation error.");
                globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Memory allocation error.\n");
                globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            }
            // In the case where we have file buffers, we already wrote the contents of buffer previously.
            if (hdfs_handle->using_file_buffer == 0) {
                memcpy(hdfs_handle->buffer+(hdfs_handle->buffer_count-1)*hdfs_handle->block_size, buffer, nbytes*sizeof(globus_byte_t));
            }
            hdfs_handle->nbytes[hdfs_handle->buffer_count-1] = nbytes;
            hdfs_handle->offsets[hdfs_handle->buffer_count-1] = offset;
        }
    }

    return rc;
}

/**
 * Scan through all the buffers we own, then write out all the consecutive ones to HDFS.
 */
globus_result_t
hdfs_dump_buffers(hdfs_handle_t *hdfs_handle) {

    globus_off_t * offsets = hdfs_handle->offsets;
    globus_size_t * nbytes = hdfs_handle->nbytes;
    globus_size_t bytes_written = 0;
    size_t i, wrote_something;
    size_t cnt = hdfs_handle->buffer_count;
    GlobusGFSName(globus_l_gfs_hdfs_dump_buffers);

    globus_result_t rc = GLOBUS_SUCCESS;

    wrote_something=1;
    // Loop through all our buffers; loop again if we write something.
    while (wrote_something == 1) {
        wrote_something=0;
        // For each of our buffers.
        for (i=0; i<cnt; i++) {
            if (hdfs_handle->used[i] == 1 && offsets[i] == hdfs_handle->offset) {
                //printf("Flushing %d bytes at offset %d from buffer %d.\n", nbytes[i], hdfs_handle->offset, i);
                if (hdfs_handle->syslog_host != NULL) {
                    syslog(LOG_INFO, hdfs_handle->syslog_msg, "WRITE", nbytes[i], hdfs_handle->offset);
                }
                bytes_written = hdfsWrite(hdfs_handle->fs, hdfs_handle->fd, hdfs_handle->buffer+i*hdfs_handle->block_size, nbytes[i]*sizeof(globus_byte_t));
                if (bytes_written > 0) {
                    wrote_something = 1;
                }
                if (bytes_written != nbytes[i]) {
                    SystemError(hdfs_handle, "Write into HDFS failed", rc);
                    set_done(hdfs_handle, rc);
                    return rc;
                }
                hdfs_handle->used[i] = 0;
                hdfs_handle->offset += bytes_written;
            }
        }
    }
    //if (hdfs_handle->buffer_count > 10) {
    //    printf("Waiting on buffer %d\n", hdfs_handle->offset);
    //}
    return rc;
}
