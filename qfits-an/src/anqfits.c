/*
 This file was added by the Astrometry.net team.
 Copyright 2007,2010 Dustin Lang.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include "anqfits.h"
#include "qfits_std.h"
#include "qfits_error.h"
#include "qfits_tools.h"
#include "qfits_table.h"
#include "qfits_memory.h"
#include "qfits_rw.h"
#include "qfits_card.h"

#define qdebug( code ) { code }

//#define debug printf
#define debug(args...)

int anqfits_n_ext(const anqfits_t* qf) {
    return qf->Nexts;
}

off_t anqfits_header_start(const anqfits_t* qf, int ext) {
	assert(ext >= 0 && ext < qf->Nexts);
    if (ext >= qf->Nexts)
        return -1;
    return (off_t)qf->exts[ext].hdr_start * FITS_BLOCK_SIZE;
}

off_t anqfits_header_size(const anqfits_t* qf, int ext) {
	assert(ext >= 0 && ext < qf->Nexts);
    if (ext >= qf->Nexts)
        return -1;
    return (off_t)qf->exts[ext].hdr_size * FITS_BLOCK_SIZE;
}

off_t anqfits_data_start(const anqfits_t* qf, int ext) {
	assert(ext >= 0 && ext < qf->Nexts);
    if (ext >= qf->Nexts)
        return -1;
    return (off_t)qf->exts[ext].data_start * FITS_BLOCK_SIZE;
}

off_t anqfits_data_size(const anqfits_t* qf, int ext) {
	assert(ext >= 0 && ext < qf->Nexts);
    if (ext >= qf->Nexts)
        return -1;
    return (off_t)qf->exts[ext].data_size * FITS_BLOCK_SIZE;
}


qfits_header* anqfits_get_header(const anqfits_t* qf, int ext) {
	assert(ext >= 0 && ext < qf->Nexts);
	if (!qf->exts[ext].header)
		qf->exts[ext].header = qfits_header_readext(qf->filename, ext);
	if (!qf->exts[ext].header)
		return NULL;
	// .....!
	return qfits_header_copy(qf->exts[ext].header);
}

qfits_table* anqfits_get_table(const anqfits_t* qf, int ext) {
	assert(ext >= 0 && ext < qf->Nexts);
	if (!qf->exts[ext].table)
		qf->exts[ext].table = qfits_table_open(qf->filename, ext);
	if (!qf->exts[ext].table)
		return NULL;
	// .....!
	return qfits_table_copy(qf->exts[ext].table);
}



static int starts_with(const char* str, const char* start) {
    int len = strlen(start);
    return strncmp(str, start, len) == 0;
}

// from qfits_cache.c: qfits_cache_add()
anqfits_t* anqfits_open(const char* filename) {
    anqfits_t* qf = NULL;
    FILE* in = NULL;
    struct stat sta;
    int n_blocks;
    int found_it;
    int xtend;
    int naxis;
    int data_bytes;
    int end_of_file;
    int skip_blocks;
    char buf[FITS_BLOCK_SIZE];
    char* buf_c;
    int seeked;
	int firsttime;
	int i;
	char* read_val;

	// initial maximum number of extensions: we grow automatically
	int off_size = 1024;

    /* Stat file to get its size */
    if (stat(filename, &sta)!=0) {
        qdebug(printf("anqfits: cannot stat file %s: %s\n", filename, strerror(errno)););
        goto bailout;
    }

    /* Open input file */
    in=fopen(filename, "r");
    if (!in) {
        qdebug(printf("anqfits: cannot open file %s: %s\n", filename, strerror(errno)););
        goto bailout;
    }

    /* Read first block in */
    if (fread(buf, 1, FITS_BLOCK_SIZE, in) != FITS_BLOCK_SIZE) {
        qdebug(printf("anqfits: error reading first block from %s: %s\n", filename, strerror(errno)););
        goto bailout;
    }
    /* Identify FITS magic number */
    if (!starts_with(buf, "SIMPLE  =")) {
        qdebug(printf("anqfits: file %s is not FITS\n", filename););
        goto bailout;
    }

    /*
     * Browse through file to identify primary HDU size and see if there
     * might be some extensions. The size of the primary data zone will
     * also be estimated from the gathering of the NAXIS?? values and
     * BITPIX.
     */

    n_blocks = 0;
    found_it = 0;
    xtend = 0;
    naxis = 0;
    data_bytes = 1;
	firsttime = 1;

    // Start looking for END card
    while (!found_it) {
		debug("Firsttime = %i\n", firsttime);
		if (!firsttime) {
			// Read next FITS block
			debug("Reading next FITS block\n");
			if (fread(buf, 1, FITS_BLOCK_SIZE, in) != FITS_BLOCK_SIZE) {
				qdebug(printf("anqfits: error reading file %s\n", filename););
				goto bailout;
			}
		}
		firsttime = 0;
        n_blocks++;
        // Browse through current block
        buf_c = buf;
        for (i=0; i<FITS_NCARDS; i++) {
			debug("Looking at line %i:\n  %.80s\n", i, buf_c);
            /* Look for BITPIX keyword */
            if (starts_with(buf_c, "BITPIX ")) {
                read_val = qfits_getvalue(buf_c);
                data_bytes *= atoi(read_val) / 8;
                if (data_bytes<0) data_bytes *= -1;

            /* Look for NAXIS keyword */
            } else if (starts_with(buf_c, "NAXIS")) {
                if (buf_c[5] == ' ') {
                    /* NAXIS keyword */
                    read_val = qfits_getvalue(buf_c);
                    naxis = atoi(read_val);
                } else {
                    /* NAXIS?? keyword (axis size) */
                    read_val = qfits_getvalue(buf_c);
                    data_bytes *= atoi(read_val);
                }

            /* Look for EXTEND keyword */
            } else if (starts_with(buf_c, "EXTEND ")) {
                /* The EXTEND keyword is present: might be some extensions */
                read_val = qfits_getvalue(buf_c);
                if (read_val[0]=='T' || read_val[0]=='1') {
                    xtend=1;
                }

            /* Look for END keyword */
            } else if (starts_with(buf_c, "END ")) {
                found_it = 1;
				break;
            }
            buf_c += FITS_LINESZ;
        }
    }

    //qf->inode= sta.st_ino;

    qf = calloc(1, sizeof(anqfits_t));
    qf->filename = strdup(filename);
	qf->exts = calloc(off_size, sizeof(anqfits_ext_t));
	assert(qf->exts);
	if (!qf->exts)
		goto bailout;

    // Set first HDU offsets
    qf->exts[0].hdr_start = 0;
    qf->exts[0].data_start = n_blocks;
	qf->Nexts = 1;
    
    if (xtend) {
        /* Look for extensions */
        qdebug(printf("anqfits: searching for extensions in %s\n", filename););

        /*
         * Register all extension offsets
         */
        end_of_file = 0;
        while (!end_of_file) {
            /*
             * Skip the previous data section if pixels were declared
             */
            if (naxis > 0) {
                /* Skip as many blocks as there are declared pixels */
                skip_blocks = data_bytes / FITS_BLOCK_SIZE;
                if (data_bytes % FITS_BLOCK_SIZE) {
                    skip_blocks++;
                }
                seeked = fseek(in, skip_blocks*FITS_BLOCK_SIZE, SEEK_CUR);
                if (seeked == -1) {
                    qdebug(printf("anqfits: error seeking file %s\n", filename););
                    goto bailout;
                }
                /* Increase counter of current seen blocks. */
                n_blocks += skip_blocks;
            }
            
            /* Look for extension start */
            found_it = 0;
            while (!found_it && !end_of_file) {
                if (fread(buf, 1, FITS_BLOCK_SIZE, in) != FITS_BLOCK_SIZE) {
                    /* Reached end of file */
                    end_of_file = 1;
                    break;
                }
                n_blocks++;

                /* Search for XTENSION at block top */
                if (starts_with(buf, "XTENSION=")) {
                    /* Got an extension */
                    found_it = 1;
                    qf->exts[qf->Nexts].hdr_start = n_blocks-1;
                }
            }
            if (end_of_file)
                break;

            // Look for extension END
            n_blocks--;
            found_it = 0;
            data_bytes = 1;
            naxis = 0;
			firsttime = 1;
            while (!found_it && !end_of_file) {
				if (!firsttime) {
					if (fread(buf, 1, FITS_BLOCK_SIZE, in) != FITS_BLOCK_SIZE) {
						qdebug(printf("anqfits: XTENSION without END in %s\n", filename););
						end_of_file = 1;
						break;
					}
				}
				firsttime = 0;
                n_blocks++;

                /* Browse current block */
                buf_c = buf;
                for (i=0; i<FITS_NCARDS; i++) {
                    /* Look for BITPIX keyword */
                    if (starts_with(buf_c, "BITPIX ")) {
                        read_val = qfits_getvalue(buf_c);
                        data_bytes *= atoi(read_val) / 8 ;
                        if (data_bytes<0)
                            data_bytes *= -1;

                    /* Look for NAXIS keyword */
                    } else if (starts_with(buf_c, "NAXIS")) {
                        if (buf_c[5]==' ') {
                            /* NAXIS keyword */
                            read_val = qfits_getvalue(buf_c);
                            naxis = atoi(read_val);
                        } else {
                            /* NAXIS?? keyword (axis size) */
                            read_val = qfits_getvalue(buf_c);
                            data_bytes *= atoi(read_val);
                        }

                    /* Look for END keyword */
                    } else if (starts_with(buf_c, "END ")) {
                        /* Got the END card */
                        found_it = 1;
						break;
					}
                    buf_c += FITS_LINESZ;
				}
			}
			if (found_it) {
				qf->exts[qf->Nexts].data_start = n_blocks;
				qf->Nexts++;
				if (qf->Nexts >= off_size) {
					off_size *= 2;
					qf->exts = realloc(qf->exts, off_size * sizeof(anqfits_ext_t));
					assert(qf->exts);
					if (!qf->exts)
						goto bailout;
				}
            }
        }
    }

    /* Close file */
    fclose(in);
    in = NULL;

    // realloc
	qf->exts = realloc(qf->exts, qf->Nexts * sizeof(anqfits_ext_t));
	assert(qf->exts);
	if (!qf->exts)
		goto bailout;

    for (i=0; i<qf->Nexts; i++) {
        qf->exts[i].hdr_size = qf->exts[i].data_start - qf->exts[i].hdr_start;
        if (i == qf->Nexts-1)
            qf->exts[i].data_size = (sta.st_size/FITS_BLOCK_SIZE) - qf->exts[i].data_start;
		else
			qf->exts[i].data_size = qf->exts[i+1].hdr_start - qf->exts[i].data_start;
    }
    qf->filesize = sta.st_size / FITS_BLOCK_SIZE;

    /* Add last modification date
     qc->mtime = sta.st_mtime ;
     qc->filesize  = sta.st_size ;
     qc->ctime = sta.st_ctime ;
     */
    return qf;

 bailout:
    if (in)
        fclose(in);
    if (qf) {
        free(qf->filename);
		free(qf->exts);
        free(qf);
    }
    return NULL;
}

void anqfits_close(anqfits_t* qf) {
	int i;
    if (!qf)
        return;
	for (i=0; i<qf->Nexts; i++) {
		if (qf->exts[i].header)
			qfits_header_destroy(qf->exts[i].header);
		if (qf->exts[i].table)
			qfits_table_close(qf->exts[i].table);
	}
	free(qf->exts);
	free(qf->filename);
    free(qf);
}





















/*
char* qfits_query_ext_2(const qfits_t* qf, int ext, const char* keyword) {
    qfits_header* hdr;
    char* str;
    // HACK - return pointer to static memory!!
    static char val[81];
    hdr = qfits_get_header(qf, ext);
    if (!hdr)
        return NULL;
    str = qfits_header_getstr(hdr, keyword);
    if (!str)
        return NULL;
    strncpy(val, str, sizeof(val));
    val[sizeof(val)-1]='\0';
    qfits_header_destroy(hdr);
    return val;
}
int qfits_is_table_2(const qfits_t* qf, int ext) {
    qfits_header* hdr;
    int ttype;
    hdr = qfits_get_header(qf, ext);
    ttype = qfits_is_table_header(hdr);
    qfits_header_destroy(hdr);
    return ttype;
}
 */

#if 0
qfits_table* anqfits_get_table(const anqfits_t* qf, int ext) {
    qfits_table* table;
    qfits_col* curr_col;
    char            *   str_val ;
    char                keyword[FITSVALSZ] ;
    /* Table infos  */
    int                 table_type ;
    int                 nb_col ;
    int                 table_width ;
    int                 nb_rows ;
    /* Column infos */
    char                label[FITSVALSZ] ;
    char                unit[FITSVALSZ] ;
    char                disp[FITSVALSZ] ;
    char                nullval[FITSVALSZ] ;
    int                 atom_nb ;
    int                 atom_dec_nb ;
    int                 atom_size ;
    tfits_type          atom_type ;
    int                 offset_beg ;
    int                 data_size ;
    int                 theory_size ;
    int                 zero_present ;
    int                 scale_present ;
    float               zero ;
    float               scale ;
    
    /* For ASCII tables */
    int                    col_pos ;    
    int                    next_col_pos ;
    
    /* For X type */
    int                    nb_bits ;
        
    int                    i ;

    qfits_header* hdr;

    if (!qf)
        return NULL;

    hdr = anqfits_get_header(qf, ext);
    if (!hdr)
        return NULL;
        
    /* Identify a table and get the table type : ASCII or BIN */
	if ((table_type = qfits_is_table_header(hdr))==QFITS_INVALIDTABLE) {
        qfits_error("[%s] extension %d is not a table", qf->filename, ext) ;
        qfits_header_destroy(hdr);
        return NULL;
    }
    
    /* Get number of columns and allocate them: nc <-> TFIELDS */
    nb_col = qfits_header_getint(hdr, "TFIELDS", -1);
    if (nb_col == -1) {
        qfits_error("cannot read TFIELDS in [%s]:[%d]", qf->filename, ext);
        qfits_header_destroy(hdr);
        return NULL;
    }

    /* Get the width in bytes of the table */
    table_width = qfits_header_getint(hdr, "NAXIS1", -1);
    if (table_width == -1) {
        qfits_error("cannot read NAXIS1 in [%s]:[%d]", qf->filename, ext) ;
        qfits_header_destroy(hdr);
        return NULL;
    }
    
	/* Get the number of rows */
    nb_rows = qfits_header_getint(hdr, "NAXIS2", -1);
    if (nb_rows == -1) {
        qfits_error("cannot read NAXIS2 in [%s]:[%d]", qf->filename, ext) ;
        qfits_header_destroy(hdr);
        return NULL;
    }

    /* Create the table object */
    table = qfits_table_new(qf->filename, table_type, table_width, nb_col, nb_rows);
    
    /* Initialize offset_beg */
    offset_beg = anqfits_data_start(qf, ext);
    data_size = anqfits_data_size(qf, ext);
    if ((offset_beg == -1) || (data_size == -1)) {
        qfits_error("cannot find data start in [%s]:[%d]", qf->filename, ext);
        qfits_header_destroy(hdr);
        qfits_table_close(table);
        return NULL;
    }
    
    /* Loop on all columns and get column descriptions  */
    curr_col = table->col;
    for (i=0; i<table->nc; i++) {
        /* label <-> TTYPE     */
        sprintf(keyword, "TTYPE%d", i+1);
        if ((str_val=qfits_header_getstr(hdr, keyword)) == NULL) {
            label[0] = '\0';
        } else strcpy(label, qfits_pretty_string(str_val));
        
        /* unit <-> TUNIT */
        sprintf(keyword, "TUNIT%d", i+1);
        if ((str_val=qfits_header_getstr(hdr, keyword)) == NULL) {
            unit[0] = '\0';
        } else strcpy(unit, qfits_pretty_string(str_val));

        /* disp <-> TDISP */
        sprintf(keyword, "TDISP%d", i+1);
        if ((str_val=qfits_header_getstr(hdr, keyword)) == NULL) {
            disp[0] = '\0';
        } else strcpy(disp, qfits_pretty_string(str_val));

        /* nullval <-> TNULL */
        sprintf(keyword, "TNULL%d", i+1);
        if ((str_val=qfits_header_getstr(hdr, keyword)) == NULL) {
            nullval[0] = '\0';
        } else strcpy(nullval, qfits_pretty_string(str_val));
    
        /* atom_size, atom_nb, atom_dec_nb, atom_type    <-> TFORM */
        sprintf(keyword, "TFORM%d", i+1);
        if ((str_val=qfits_header_getstr(hdr, keyword)) == NULL) {
            qfits_error("cannot read [%s] in [%s]:[%d]", keyword, qf->filename, ext);
            qfits_header_destroy(hdr);
            qfits_table_close(table);
            return NULL;
        }
        /* Interpret the type in header */
        if (qfits_table_interpret_type(qfits_pretty_string(str_val), 
                        &(atom_nb), 
                        &(atom_dec_nb),
                        &(atom_type), 
                        table_type) == -1) {
            qfits_error("cannot interpret the type: %s", str_val) ;
            qfits_table_close(table) ;
            qfits_header_destroy(hdr);
            return NULL ;
        }
        
        /* Set atom_size */
        switch (atom_type) {
            case TFITS_BIN_TYPE_A:
            case TFITS_BIN_TYPE_L:
            case TFITS_BIN_TYPE_B:
                atom_size = 1 ;
                break ;
            case TFITS_BIN_TYPE_I:
                atom_size = 2 ;
                break ;
            case TFITS_BIN_TYPE_J:
            case TFITS_BIN_TYPE_E:
            case TFITS_ASCII_TYPE_I:
            case TFITS_ASCII_TYPE_E:
            case TFITS_ASCII_TYPE_F:
                atom_size = 4 ;
                break ;
            case TFITS_BIN_TYPE_C:
            case TFITS_BIN_TYPE_P:
                atom_size = 4 ;
                atom_nb *= 2 ;
                break ;
            case TFITS_BIN_TYPE_K:
            case TFITS_BIN_TYPE_D:
            case TFITS_ASCII_TYPE_D:
                atom_size = 8 ;
                break ;
            case TFITS_BIN_TYPE_M:
                atom_size = 8 ;
                atom_nb *= 2 ;
                break ;
            case TFITS_BIN_TYPE_X:
                atom_size = 1 ;
                nb_bits = atom_nb ;
                atom_nb = (int)((nb_bits - 1)/ 8) + 1 ;
                break ;
            case TFITS_ASCII_TYPE_A:
                atom_size = atom_nb ;
                break ;
            default:
                qfits_error("unrecognized type") ;
                qfits_table_close(table) ;
                qfits_header_destroy(hdr);
                return NULL;
        }
    
        /* zero <-> TZERO */
        sprintf(keyword, "TZERO%d", i+1);
        if ((str_val=qfits_header_getstr(hdr, keyword)) == NULL) {
            zero = (float)atof(str_val) ;
            zero_present = 1 ;    
        } else {
            zero = (float)0.0 ;
            zero_present = 0 ;    
        }
        
        /* scale <-> TSCAL */
        sprintf(keyword, "TSCAL%d", i+1);
        if ((str_val=qfits_header_getstr(hdr, keyword)) == NULL) {
            scale = (float)atof(str_val) ;
            scale_present = 1 ;
        } else {
            scale = (float)1.0 ;
            scale_present = 0 ;
        }

        /* Fill the current column object */
        qfits_col_fill(curr_col, atom_nb, atom_dec_nb, atom_size, atom_type, 
                label, unit, nullval, disp, zero_present, zero, scale_present, 
                scale, offset_beg) ;
        
        /* Compute offset_beg but for the last column */
        if (i < table->nc - 1) {
            if (table_type == QFITS_ASCIITABLE) {
                /* column width <-> TBCOLi and TBCOLi+1 */
                sprintf(keyword, "TBCOL%d", i+1);
                col_pos = qfits_header_getint(hdr, keyword, -1);
                if (col_pos == -1) {
                    qfits_error("cannot read [%s] in [%s]", keyword, qf->filename);
                    qfits_table_close(table);
                    qfits_header_destroy(hdr);
                    return NULL;
                }
                
                sprintf(keyword, "TBCOL%d", i+2) ;
                next_col_pos = qfits_header_getint(hdr, keyword, -1);
                if (next_col_pos == -1) {
                    qfits_error("cannot read [%s] in [%s]", keyword, qf->filename) ;
                    qfits_table_close(table) ;
                    qfits_header_destroy(hdr);
                    return NULL;
                }
                offset_beg += (int)(next_col_pos - col_pos);
            } else if (table_type == QFITS_BINTABLE) {
                offset_beg += atom_nb * atom_size;
            }
        }
        curr_col++;
    }
    qfits_header_destroy(hdr);

    /* Check that the theoretical data size is not far from the measured */
    /* one by more than 2880 */
    theory_size = qfits_compute_table_width(table)*table->nr;
    if (data_size < theory_size) {
        qfits_error("Inconsistent data sizes");
        qfits_table_close(table);
        return NULL;
    }
    
    return table;
}
#endif
#if 0
qfits_header* anqfits_get_header(const anqfits_t* qf, int ext) {
    int start;
    size_t size;
    char* map;
    qfits_header* hdr;
    char line[81];
	char* where;
	char* key;
	char* val;
	char* com;

    if (!qf)
        return NULL;
    start = anqfits_header_start(qf, ext);
    size  = anqfits_header_size (qf, ext);
    if ((start == -1) || (size == -1))
        return NULL;

    /* Memory-map the input file */
    map = qfits_falloc(qf->filename, start, &size);
    if (!map) {
		qfits_error("qfits_falloc failed; maybe you're out of memory (or address space)?");
		return NULL;
	}

    hdr = qfits_header_new();
    where = map;
    while (1) {
        memcpy(line, where, 80);
        line[80] = '\0';

        /* Rule out blank lines */
        if (!is_blank_line(line)) {
            /* Get key, value, comment for the current line */
            key = qfits_getkey(line);
            val = qfits_getvalue(line);
            com = qfits_getcomment(line);

            /* If key or value cannot be found, trigger an error */
            if (!key) {
                qfits_header_destroy(hdr);
                hdr = NULL;
                break;
            }
            /* Append card to linked-list */
            qfits_header_append(hdr, key, val, com, line);
            /* Check for END keyword */
            if (strcmp(key, "END") == 0)
                break ;
        }
        where += 80;
        /* If reaching the end of file, trigger an error */
        if ((int)(where-map) >= (int)(seg_size+80)) {
            qfits_header_destroy(hdr);
            hdr = NULL;
            break;
        }
    }
    qfits_fdealloc(map, start, size);
    return hdr;
}
#endif
