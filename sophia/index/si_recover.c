
/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

/*
	repository recover states
	-------------------------

	compaction

	000000001.000000002.db.incomplete  (1)
	000000001.000000002.db.seal        (2)
	000000002.db                       (3)
	000000001.000000003.db.incomplete
	000000001.000000003.db.seal
	000000003.db
	(4)

	1. remove incomplete, mark parent as having incomplete
	2. find parent, mark as having seal
	3. add
	4. recover:
		a. if parent has incomplete and seal - remove both
		b. if parent has incomplete - remove incomplete
		c. if parent has seal - remove parent, complete seal
	5. panic (auto-recover)

	see: scheme recover
	see: test/crash/durability.test.c
*/

#include <libss.h>
#include <libsf.h>
#include <libsr.h>
#include <libso.h>
#include <libsv.h>
#include <libsd.h>
#include <libsi.h>

sinode *si_bootstrap(si *i, uint64_t parent)
{
	sr *r = &i->r;
	/* create node */
	uint64_t id = sr_seq(r->seq, SR_NSNNEXT);
	sinode *n = si_nodenew(r, id, parent);
	if (ssunlikely(n == NULL))
		return NULL;
	int rc;
	rc = si_nodecreate(n, r, &i->scheme);
	if (ssunlikely(rc == -1))
		goto e0;

	/* create index with one empty page */
	sdbuild build;
	sd_buildinit(&build);
	sdindex index;
	sd_indexinit(&index);
	sdbuildindex build_index;
	sd_buildindex_init(&build_index);

	rc = sd_buildindex_begin(&build_index);
	if (ssunlikely(rc == -1))
		goto e0;

	sdio io;
	sd_ioinit(&io);
	if (i->scheme.direct_io) {
		rc = sd_ioprepare(&io, r,
		                  i->scheme.direct_io,
		                  i->scheme.direct_io_page_size,
		                  i->scheme.direct_io_buffer_size);
		if (ssunlikely(rc == -1))
			goto e1;
	}
	rc = sd_buildbegin(&build, r,
	                   i->scheme.compaction.node_page_checksum,
	                   i->scheme.compression,
	                   i->scheme.compression_if);
	if (ssunlikely(rc == -1))
		goto e1;
	sd_buildend(&build, r);
	rc = sd_buildindex_add(&build_index, r, &build, 0);
	if (ssunlikely(rc == -1))
		goto e1;

	/* write page */
	rc = sd_writepage(r, &n->file, &io, &build);
	if (ssunlikely(rc == -1))
		goto e1;
	uint32_t align = 0;
	if (i->scheme.direct_io)
		align = i->scheme.direct_io_page_size;
	rc = sd_buildindex_end(&build_index, r, align, sd_iosize(&io, &n->file));
	if (ssunlikely(rc == -1))
		goto e1;

	/* write index */
	rc = sd_indexcopy_buf(&index, r, &build_index.v, &build_index.m);
	if (ssunlikely(rc == -1))
		goto e1;
	rc = sd_writeindex(r, &n->file, &io, &index);
	if (ssunlikely(rc == -1))
		goto e1;
	if (i->scheme.mmap) {
		rc = si_nodemap(n, r);
		if (ssunlikely(rc == -1))
			goto e1;
	}
	n->index = index;

	sd_iofree(&io, r);
	sd_buildfree(&build, r);
	sd_buildindex_free(&build_index, r);
	return n;
e1:
	sd_iofree(&io, r);
	sd_indexfree(&index, r);
	sd_buildfree(&build, r);
	sd_buildindex_free(&build_index, r);
e0:
	si_nodefree(n, r, 0);
	return NULL;
}

static inline int
si_deploy(si *i, sr *r, int create_directory)
{
	/* create directory */
	int rc;
	if (sslikely(create_directory)) {
		rc = ss_vfsmkdir(r->vfs, i->scheme.path, 0755);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "directory '%s' create error: %s",
			               i->scheme.path, strerror(errno));
			return -1;
		}
	}
	/* create scheme file */
	rc = si_schemedeploy(&i->scheme, r);
	if (ssunlikely(rc == -1)) {
		sr_malfunction_set(r->e);
		return -1;
	}
	/* create initial node */
	sinode *n = si_bootstrap(i, 0);
	if (ssunlikely(n == NULL))
		return -1;
	SS_INJECTION(r->i, SS_INJECTION_SI_RECOVER_0,
	             si_nodefree(n, r, 0);
	             sr_malfunction(r->e, "%s", "error injection");
	             return -1);
	rc = si_noderename_complete(n, r, &i->scheme);
	if (ssunlikely(rc == -1)) {
		si_nodefree(n, r, 1);
		return -1;
	}
	si_insert(i, n);
	si_plannerupdate(&i->p, n);
	return 1;
}

static inline int64_t
si_processid(char **str)
{
	char *s = *str;
	size_t v = 0;
	while (*s && *s != '.') {
		if (ssunlikely(! isdigit(*s)))
			return -1;
		v = (v * 10) + *s - '0';
		s++;
	}
	*str = s;
	return v;
}

static inline int
si_process(char *name, uint64_t *nsn, uint64_t *parent)
{
	/* id.db */
	/* id.id.db.incomplete */
	/* id.id.db.seal */
	/* id.id.db.gc */
	char *token = name;
	int64_t id = si_processid(&token);
	if (ssunlikely(id == -1))
		return -1;
	*parent = id;
	*nsn = id;
	if (strcmp(token, ".db") == 0)
		return SI_RDB;
	else
	if (strcmp(token, ".db.gc") == 0)
		return SI_RDB_REMOVE;
	if (ssunlikely(*token != '.'))
		return -1;
	token++;
	id = si_processid(&token);
	if (ssunlikely(id == -1))
		return -1;
	*nsn = id;
	if (strcmp(token, ".db.incomplete") == 0)
		return SI_RDB_DBI;
	else
	if (strcmp(token, ".db.seal") == 0)
		return SI_RDB_DBSEAL;
	return -1;
}

static inline int
si_trackdir(sitrack *track, sr *r, si *i)
{
	DIR *dir = opendir(i->scheme.path);
	if (ssunlikely(dir == NULL)) {
		sr_malfunction(r->e, "directory '%s' open error: %s",
		               i->scheme.path, strerror(errno));
		return -1;
	}
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (ssunlikely(de->d_name[0] == '.'))
			continue;
		uint64_t id_parent = 0;
		uint64_t id = 0;
		int rc = si_process(de->d_name, &id, &id_parent);
		if (ssunlikely(rc == -1))
			continue; /* skip unknown file */
		si_tracknsn(track, id_parent);
		si_tracknsn(track, id);

		sinode *head, *node;
		sspath path;
		switch (rc) {
		case SI_RDB_DBI:
		case SI_RDB_DBSEAL: {
			/* find parent node and mark it as having
			 * incomplete compaction process */
			head = si_trackget(track, id_parent);
			if (sslikely(head == NULL)) {
				head = si_nodenew(r, id_parent, UINT64_MAX);
				if (ssunlikely(head == NULL))
					goto error;
				head->recover = SI_RDB_UNDEF;
				si_trackset(track, head);
			}
			head->recover |= rc;
			/* remove any incomplete file made during compaction */
			if (rc == SI_RDB_DBI) {
				ss_pathcompound(&path, i->scheme.path, id_parent, id,
				                ".db.incomplete");
				rc = ss_vfsunlink(r->vfs, path.path);
				if (ssunlikely(rc == -1)) {
					sr_malfunction(r->e, "db file '%s' unlink error: %s",
					               path.path, strerror(errno));
					goto error;
				}
				continue;
			}
			assert(rc == SI_RDB_DBSEAL);
			/* recover 'sealed' node */
			node = si_nodenew(r, id, id_parent);
			if (ssunlikely(node == NULL))
				goto error;
			node->recover = SI_RDB_DBSEAL;
			ss_pathcompound(&path, i->scheme.path, id_parent, id,
			                ".db.seal");
			rc = si_nodeopen(node, r, &i->scheme, &path);
			if (ssunlikely(rc == -1)) {
				si_nodefree(node, r, 0);
				goto error;
			}
			si_trackset(track, node);
			si_trackmetrics(track, node);
			continue;
		}
		case SI_RDB_REMOVE:
			ss_path(&path, i->scheme.path, id, ".db.gc");
			rc = ss_vfsunlink(r->vfs, ss_pathof(&path));
			if (ssunlikely(rc == -1)) {
				sr_malfunction(r->e, "db file '%s' unlink error: %s",
				               ss_pathof(&path), strerror(errno));
				goto error;
			}
			continue;
		}
		assert(rc == SI_RDB);


		/* recover node */
		node = si_nodenew(r, id, id_parent);
		if (ssunlikely(node == NULL))
			goto error;
		node->recover = SI_RDB;
		ss_path(&path, i->scheme.path, id, ".db");
		rc = si_nodeopen(node, r, &i->scheme, &path);
		if (ssunlikely(rc == -1)) {
			si_nodefree(node, r, 0);
			goto error;
		}
		si_trackmetrics(track, node);

		/* track node */
		head = si_trackget(track, id);
		if (sslikely(head == NULL)) {
			si_trackset(track, node);
		} else {
			/* replace a node previously created by a
			 * incomplete compaction */
			si_trackreplace(track, head, node);
			head->recover &= ~SI_RDB_UNDEF;
			node->recover |= head->recover;
			si_nodefree(head, r, 0);
		}
	}
	closedir(dir);
	return 0;
error:
	closedir(dir);
	return -1;
}

static inline int
si_trackvalidate(sitrack *track, ssbuf *buf, sr *r, si *i)
{
	ss_bufreset(buf);
	ssrbnode *p = ss_rbmax(&track->i);
	while (p) {
		sinode *n = sscast(p, sinode, node);
		switch (n->recover) {
		case SI_RDB|SI_RDB_DBI|SI_RDB_DBSEAL|SI_RDB_REMOVE:
		case SI_RDB|SI_RDB_DBSEAL|SI_RDB_REMOVE:
		case SI_RDB|SI_RDB_REMOVE:
		case SI_RDB_UNDEF|SI_RDB_DBSEAL|SI_RDB_REMOVE:
		case SI_RDB|SI_RDB_DBI|SI_RDB_DBSEAL:
		case SI_RDB|SI_RDB_DBI:
		case SI_RDB:
		case SI_RDB|SI_RDB_DBSEAL:
		case SI_RDB_UNDEF|SI_RDB_DBSEAL: {
			/* match and remove any leftover ancestor */
			sinode *ancestor = si_trackget(track, n->id_parent);
			if (ancestor && (ancestor != n))
				ancestor->recover |= SI_RDB_REMOVE;
			break;
		}
		case SI_RDB_DBSEAL: {
			/* find parent */
			sinode *parent = si_trackget(track, n->id_parent);
			if (parent) {
				/* schedule node for removal, if has incomplete merges */
				if (parent->recover & SI_RDB_DBI)
					n->recover |= SI_RDB_REMOVE;
				else
					parent->recover |= SI_RDB_REMOVE;
			}
			if (! (n->recover & SI_RDB_REMOVE)) {
				/* complete node */
				int rc = si_noderename_complete(n, r, &i->scheme);
				if (ssunlikely(rc == -1))
					return -1;
				n->recover = SI_RDB;
			}
			break;
		}
		default:
			/* corrupted states */
			return sr_malfunction(r->e, "corrupted database repository: %s",
			                      i->scheme.path);
		}
		p = ss_rbprev(&track->i, p);
	}
	return 0;
}

static inline int
si_recovercomplete(sitrack *track, sr *r, si *index, ssbuf *buf)
{
	/* prepare and build primary index */
	ss_bufreset(buf);
	ssrbnode *p = ss_rbmin(&track->i);
	while (p) {
		sinode *n = sscast(p, sinode, node);
		int rc = ss_bufadd(buf, r->a, &n, sizeof(sinode*));
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
		p = ss_rbnext(&track->i, p);
	}
	ssiter i;
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, buf, sizeof(sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		sinode *n = ss_iterof(ss_bufiterref, &i);
		if (n->recover & SI_RDB_REMOVE) {
			int rc = si_nodefree(n, r, 1);
			if (ssunlikely(rc == -1))
				return -1;
			ss_iternext(ss_bufiterref, &i);
			continue;
		}
		n->recover = SI_RDB;
		si_insert(index, n);
		si_plannerupdate(&index->p, n);
		ss_iternext(ss_bufiterref, &i);
	}
	return 0;
}

static inline int
si_recoverindex(si *i, sr *r)
{
	sitrack track;
	si_trackinit(&track);
	ssbuf buf;
	ss_bufinit(&buf);
	int rc;
	rc = si_trackdir(&track, r, i);
	if (ssunlikely(rc == -1))
		goto error;
	if (ssunlikely(track.count == 0))
		return 1;
	rc = si_trackvalidate(&track, &buf, r, i);
	if (ssunlikely(rc == -1))
		goto error;
	rc = si_recovercomplete(&track, r, i, &buf);
	if (ssunlikely(rc == -1))
		goto error;
	/* set actual metrics */
	if (track.nsn > r->seq->nsn)
		r->seq->nsn = track.nsn;
	if (track.lsn > r->seq->lsn)
		r->seq->lsn = track.lsn;
	ss_buffree(&buf, r->a);
	return 0;
error:
	ss_buffree(&buf, r->a);
	si_trackfree(&track, r);
	return -1;
}

int si_recover(si *i)
{
	sr *r = &i->r;
	int exist = ss_vfsexists(r->vfs, i->scheme.path);
	if (exist == 0)
		goto deploy;
	int rc;
	rc = si_schemerecover(&i->scheme, r);
	if (ssunlikely(rc == -1))
		return -1;
	r->scheme = &i->scheme.scheme;
	rc = si_recoverindex(i, r);
	if (sslikely(rc <= 0))
		return rc;
deploy:
	return si_deploy(i, r, !exist);
}
