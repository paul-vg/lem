/*
 * This file is part of LEM, a Lua Event Machine.
 * Copyright 2011-2012 Emil Renner Berthing
 *
 * LEM is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * LEM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LEM.  If not, see <http://www.gnu.org/licenses/>.
 */

struct stream {
	struct ev_io r;
	struct ev_io w;
	const char *out;
	size_t out_len;
	int idx;
	struct lem_parser *p;
	struct lem_inputbuf buf;
};

#define STREAM_FROM_WATCH(w, member)\
	(struct stream *)(((char *)w) - offsetof(struct stream, member))

static struct stream *
stream_new(lua_State *T, int fd, int mt)
{
	/* create userdata and set the metatable */
	struct stream *s = lua_newuserdata(T, sizeof(struct stream));
	lua_pushvalue(T, mt);
	lua_setmetatable(T, -2);

	/* initialize userdata */
	ev_io_init(&s->r, NULL, fd, EV_READ);
	ev_io_init(&s->w, NULL, fd, EV_WRITE);
	s->r.data = NULL;
	s->w.data = NULL;
	s->buf.start = s->buf.end = 0;

	return s;
}

static int
stream_gc(lua_State *T)
{
	struct stream *s = lua_touserdata(T, 1);

	if (s->r.fd >= 0)
		close(s->r.fd);

	return 0;
}

static int
stream_closed(lua_State *T)
{
	struct stream *s;

	luaL_checktype(T, 1, LUA_TUSERDATA);
	s = lua_touserdata(T, 1);
	lua_pushboolean(T, s->r.fd < 0);
	return 1;
}

static int
stream_close(lua_State *T)
{
	struct stream *s;
	int ret;

	luaL_checktype(T, 1, LUA_TUSERDATA);
	s = lua_touserdata(T, 1);
	if (s->r.fd < 0)
		return io_closed(T);
	if (s->r.data != NULL || s->w.data != NULL)
		return io_busy(T);

	ret = close(s->r.fd);
	s->r.fd = s->w.fd = -1;
	if (ret)
		return io_strerror(T, errno);

	lua_pushboolean(T, 1);
	return 1;
}

/*
 * stream:readp() method
 */
static int
stream__readp(lua_State *T, struct stream *s)
{
	ssize_t bytes;
	int ret;
	int err;
	enum lem_preason res;

	while ((bytes = read(s->r.fd, s->buf.buf + s->buf.end,
					LEM_INPUTBUF_SIZE - s->buf.end)) > 0) {
		lem_debug("read %ld bytes from %d", bytes, s->r.fd);

		s->buf.end += bytes;

		ret = s->p->process(T, &s->buf);
		if (ret > 0)
			return ret;
	}
	err = errno;
	lem_debug("read %ld bytes from %d", bytes, s->r.fd);

	if (bytes < 0 && err == EAGAIN)
		return 0;

	if (bytes == 0 || err == ECONNRESET || err == EPIPE)
		res = LEM_PCLOSED;
	else
		res = LEM_PERROR;

	if (s->p->destroy && (ret = s->p->destroy(T, &s->buf, res)) > 0)
		return ret;

	lua_settop(T, 0);
	if (res == LEM_PCLOSED)
		return io_closed(T);

	return io_strerror(T, err);
}

static void
stream_readp_cb(EV_P_ struct ev_io *w, int revents)
{
	struct stream *s = STREAM_FROM_WATCH(w, r);
	lua_State *T = s->r.data;
	int ret;

	(void)revents;

	ret = stream__readp(T, s);
	if (ret == 0)
		return;

	ev_io_stop(EV_A_ &s->r);
	s->r.data = NULL;
	lem_queue(T, ret);
}

static int
stream_readp(lua_State *T)
{
	struct stream *s;
	struct lem_parser *p;
	int ret;

	luaL_checktype(T, 1, LUA_TUSERDATA);
	ret = lua_type(T, 2);
	if (ret != LUA_TUSERDATA && ret != LUA_TLIGHTUSERDATA)
		return luaL_argerror(T, 2, "expected userdata");

	s = lua_touserdata(T, 1);
	if (s->r.fd < 0)
		return io_closed(T);
	if (s->r.data != NULL)
		return io_busy(T);

	p = lua_touserdata(T, 2);
	if (p->init)
		p->init(T, &s->buf);

	ret = p->process(T, &s->buf);
	if (ret > 0)
		return ret;

	s->p = p;
	ret = stream__readp(T, s);
	if (ret > 0)
		return ret;

	s->r.data = T;
	s->r.cb = stream_readp_cb;
	ev_io_start(LEM_ &s->r);
	return lua_yield(T, lua_gettop(T));
}

/*
 * stream:write() method
 */
static int
stream__write(lua_State *T, struct stream *s)
{
	ssize_t bytes;
	int err;

	while ((bytes = write(s->w.fd, s->out, s->out_len)) > 0) {
		s->out += bytes;
		s->out_len -= bytes;
		if (s->out_len == 0) {
			if (s->idx == lua_gettop(T)) {
				lua_pushboolean(T, 1);
				return 1;
			}
			s->out = lua_tolstring(T, ++s->idx, &s->out_len);
		}
	}
	err = errno;

	if (bytes < 0 && err == EAGAIN)
		return 0;

	close(s->w.fd);
	s->w.fd = s->r.fd = -1;

	if (bytes == 0 || err == ECONNRESET || err == EPIPE)
		return io_closed(T);

	return io_strerror(T, err);
}

static void
stream_write_cb(EV_P_ struct ev_io *w, int revents)
{
	struct stream *s = STREAM_FROM_WATCH(w, w);
	lua_State *T = s->w.data;
	int ret;

	(void)revents;

	ret = stream__write(T, s);
	if (ret == 0)
		return;

	ev_io_stop(EV_A_ &s->w);
	s->w.data = NULL;
	lem_queue(T, ret);
}

static int
stream_write(lua_State *T)
{
	struct stream *s;
	const char *out;
	size_t out_len;
	int i;
	int top;
	int ret;

	luaL_checktype(T, 1, LUA_TUSERDATA);
	out = luaL_checklstring(T, 2, &out_len);
	top = lua_gettop(T);
	for (i = 3; i <= top; i++)
		(void)luaL_checkstring(T, i);

	s = lua_touserdata(T, 1);
	if (s->w.fd < 0)
		return io_closed(T);
	if (s->w.data != NULL)
		return io_busy(T);

	s->out = out;
	s->out_len = out_len;
	s->idx = 2;
	ret = stream__write(T, s);
	if (ret > 0)
		return ret;

	s->w.data = T;
	s->w.cb = stream_write_cb;
	ev_io_start(LEM_ &s->w);
	return lua_yield(T, top);
}

#ifndef TCP_CORK
#define TCP_CORK TCP_NOPUSH
#endif

static int
stream_setcork(lua_State *T, int state)
{
	struct stream *s;

	luaL_checktype(T, 1, LUA_TUSERDATA);
	s = lua_touserdata(T, 1);
	if (s->w.fd < 0)
		return io_closed(T);
	if (s->w.data != NULL)
		return io_busy(T);

	if (setsockopt(s->w.fd, IPPROTO_TCP, TCP_CORK, &state, sizeof(int)))
		return io_strerror(T, errno);

	lua_pushboolean(T, 1);
	return 1;
}

static int
stream_cork(lua_State *T)
{
	return stream_setcork(T, 1);
}

static int
stream_uncork(lua_State *T)
{
	return stream_setcork(T, 0);
}

static void
stream_sendfile_work(struct lem_async *a)
{
	struct file *f = (struct file *)a;
	struct stream *s = f->sendfile.stream;

	/* make socket blocking */
	if (fcntl(s->w.fd, F_SETFL, 0)) {
		f->ret = errno;
		close(s->w.fd);
		s->r.fd = s->w.fd = -1;
		return;
	}

#ifdef __FreeBSD__
	off_t written;
	int ret = sendfile(f->fd, s->w.fd,
			f->sendfile.offset, f->sendfile.size,
			NULL, &written, SF_SYNC);
	if (ret == 0) {
		f->ret = 0;
		f->sendfile.size = written;
	} else
		f->ret = errno;
	lem_debug("wrote = %ld bytes", written);
#else
#ifdef __APPLE__
	int ret = sendfile(f->fd, s->w.fd,
			f->sendfile.offset, &f->sendfile.size,
			NULL, 0);
	if (ret == 0)
		f->ret = 0;
	else
		f->ret = errno;
	lem_debug("wrote = %lld bytes", f->sendfile.size);
#else
	ssize_t ret = sendfile(s->w.fd, f->fd,
			&f->sendfile.offset, f->sendfile.size);
	if (ret >= 0) {
		f->ret = 0;
		f->sendfile.size = ret;
	} else
		f->ret = errno;
	lem_debug("wrote = %ld bytes", ret);
#endif
#endif

	/* make socket non-blocking again */
	if (fcntl(s->w.fd, F_SETFL, O_NONBLOCK)) {
		f->ret = errno;
		close(s->w.fd);
		s->r.fd = s->w.fd = -1;
		return;
	}
}

static void
stream_sendfile_reap(struct lem_async *a)
{
	struct file *f = (struct file *)a;
	struct stream *s = f->sendfile.stream;
	lua_State *T = f->a.T;
	int ret;

	if (f->ret == 0) {
		lua_pushnumber(T, f->sendfile.size);
		ret = 1;
	} else {
		ret = io_strerror(T, f->ret);
	}

	f->a.T = NULL;
	s->w.data = NULL;
	lem_queue(T, ret);
}

static int
stream_sendfile(lua_State *T)
{
	struct stream *s;
	struct file *f;
	off_t size;
	off_t offset;

	luaL_checktype(T, 1, LUA_TUSERDATA);
	luaL_checktype(T, 2, LUA_TUSERDATA);
	size = (off_t)luaL_checknumber(T, 3);
	offset = (off_t)luaL_optnumber(T, 4, 0);

	s = lua_touserdata(T, 1);
	if (s->w.fd < 0)
		return io_closed(T);
	if (s->w.data != NULL)
		return io_busy(T);

	f = lua_touserdata(T, 2);
	if (f->fd < 0) {
		lua_pushnil(T);
		lua_pushliteral(T, "file closed");
		return 2;
	}
	if (f->a.T != NULL) {
		lua_pushnil(T);
		lua_pushliteral(T, "file busy");
		return 2;
	}

	s->w.data = T;
	f->sendfile.stream = s;
	f->sendfile.size = size;
	f->sendfile.offset = offset;
	lem_async_do(&f->a, T, stream_sendfile_work, stream_sendfile_reap);

	lua_settop(T, 2);
	return lua_yield(T, 2);
}

static int
stream_popen(lua_State *T)
{
	const char *cmd = luaL_checkstring(T, 1);
	const char *mode = luaL_optstring(T, 2, "r");
	int fd[2];
	int err;

	if (mode[0] != 'r' && mode[0] != 'w')
		return luaL_error(T, "invalid mode string");

	if (pipe(fd))
		return io_strerror(T, errno);

	switch (fork()) {
	case -1: /* error */
		err = errno;
		close(fd[0]);
		close(fd[1]);
		return io_strerror(T, err);

	case 0: /* child */
		if (mode[0] == 'r') {
			close(fd[0]);
			dup2(fd[1], 1);
		} else {
			close(fd[1]);
			dup2(fd[0], 0);
		}

		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		exit(EXIT_FAILURE);
	}

	if (mode[0] == 'r') {
		if (close(fd[1])) {
			err = errno;
			close(fd[0]);
			return io_strerror(T, err);
		}
	} else {
		if (close(fd[0])) {
			err = errno;
			close(fd[1]);
			return io_strerror(T, err);
		}
		fd[0] = fd[1];
	}

	/* make the pipe non-blocking */
	if (fcntl(fd[0], F_SETFL, O_NONBLOCK) < 0) {
		err = errno;
		close(fd[0]);
		return io_strerror(T, err);
	}

	stream_new(T, fd[0], lua_upvalueindex(1));
	return 1;
}