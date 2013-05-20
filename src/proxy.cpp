// libelliptics-proxy - smart proxy for Elliptics file storage
// Copyright (C) 2012 Anton Kortunov <toshik@yandex-team.ru>

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

#include <sstream>
#include <algorithm>
#include <iterator>
#include <iostream>
#include <memory>
#include <functional>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <chrono>

#include <fcntl.h>
#include <errno.h>
#include <netdb.h>

#include <sys/socket.h>

#include <boost/lexical_cast.hpp>
#include <boost/tokenizer.hpp>

#include <elliptics/proxy.hpp>
#include <cocaine/dealer/utils/error.hpp>
#include <msgpack.hpp>

#include "utils.hpp"

namespace {

size_t uploads_need(size_t success_copies_num, size_t replication_count) {
	switch (success_copies_num) {
	case elliptics::SUCCESS_COPIES_TYPE__ANY:
		return 1;
	case elliptics::SUCCESS_COPIES_TYPE__QUORUM:
		return (replication_count >> 1) + 1;
	case elliptics::SUCCESS_COPIES_TYPE__ALL:
		return replication_count;
	default:
		return replication_count;
	}
}

bool upload_is_good(size_t success_copies_num, size_t replication_count, size_t size) {
	switch (success_copies_num) {
	case elliptics::SUCCESS_COPIES_TYPE__ANY:
		return size >= 1;
	case elliptics::SUCCESS_COPIES_TYPE__QUORUM:
		return size >= ((replication_count >> 1) + 1);
	case elliptics::SUCCESS_COPIES_TYPE__ALL:
		return size == replication_count;
	default:
		return size >= success_copies_num;
	}
}

class write_helper_t {
public:
	typedef std::vector<elliptics::lookup_result_t> LookupResults;
	typedef std::vector<int> groups_t;

	write_helper_t(int success_copies_num, int replication_count, const groups_t desired_groups)
		: success_copies_num(success_copies_num)
		, replication_count(replication_count)
		, desired_groups(desired_groups)
	{
	}

	void update_lookup(const LookupResults &tmp, bool update_ret = true) {
		groups_t groups;
		const size_t size = tmp.size();
		groups.reserve(size);

		if (update_ret) {
			ret.clear();
			ret.reserve(size);
			ret.insert(ret.end(), tmp.begin(), tmp.end());
		}

		for (auto it = tmp.begin(), end = tmp.end(); it != end; ++it) {
			groups.push_back(it->group());
		}

		upload_groups.swap(groups);
	}

	const groups_t &get_upload_groups() const {
		return upload_groups;
	}

	bool upload_is_good() const {
		return ::upload_is_good(success_copies_num, replication_count, upload_groups.size());
	}

	bool has_incomplete_groups() const {
		return desired_groups.size() != upload_groups.size();
	}

	groups_t get_incomplete_groups() {
		groups_t incomplete_groups;
		incomplete_groups.reserve(desired_groups.size() - upload_groups.size());
		std::sort(desired_groups.begin(), desired_groups.end());
		std::sort(upload_groups.begin(), upload_groups.end());
		std::set_difference(desired_groups.begin(), desired_groups.end(),
							 upload_groups.begin(), upload_groups.end(),
							 std::back_inserter(incomplete_groups));
		return incomplete_groups;
	}

	const LookupResults &get_result() const {
		return ret;
	}

private:

	int success_copies_num;
	int replication_count;
	//
	LookupResults ret;
	groups_t desired_groups;
	groups_t upload_groups;

};

} // namespace

using namespace ioremap::elliptics;

#ifdef HAVE_METABASE
namespace msgpack {
inline elliptics::group_info_response_t &operator >> (object o, elliptics::group_info_response_t &v) {
	if (o.type != type::MAP) {
		throw type_error();
	}

	msgpack::object_kv *p = o.via.map.ptr;
	msgpack::object_kv *const pend = o.via.map.ptr + o.via.map.size;

	for (; p < pend; ++p) {
		std::string key;

		p->key.convert(&key);

		//			if (!key.compare("nodes")) {
		//				p->val.convert(&(v.nodes));
		//			}
		if (!key.compare("couples")) {
			p->val.convert(&(v.couples));
		}
		else if (!key.compare("status")) {
			std::string status;
			p->val.convert(&status);
			if (!status.compare("bad")) {
				v.status = elliptics::GROUP_INFO_STATUS_BAD;
			} else if (!status.compare("coupled")) {
				v.status = elliptics::GROUP_INFO_STATUS_COUPLED;
			}
		}
	}

	return v;
}

inline elliptics::metabase_group_weights_response_t &operator >> (
		object o,
		elliptics::metabase_group_weights_response_t &v) {
	if (o.type != type::MAP) {
		throw type_error();
	}

	msgpack::object_kv *p = o.via.map.ptr;
	msgpack::object_kv *const pend = o.via.map.ptr + o.via.map.size;

	for (; p < pend; ++p) {
		elliptics::metabase_group_weights_response_t::SizedGroups sized_groups;
		p->key.convert(&sized_groups.size);
		p->val.convert(&sized_groups.weighted_groups);
		v.info.push_back(sized_groups);
	}

	return v;
}
}
#endif /* HAVE_METABASE */

namespace elliptics {

enum dnet_common_embed_types {
	DNET_PROXY_EMBED_DATA		= 1,
	DNET_PROXY_EMBED_TIMESTAMP
};

struct dnet_common_embed {
	uint64_t		size;
	uint32_t		type;
	uint32_t		flags;
	uint8_t			data[0];
};

static inline void dnet_common_convert_embedded(struct dnet_common_embed *e) {
	e->size = dnet_bswap64(e->size);
	e->type = dnet_bswap32(e->type);
	e->flags = dnet_bswap32(e->flags);
}

class elliptics_proxy_t::impl {
public:
	impl(const elliptics_proxy_t::config &c);
	~impl();

	lookup_result_t lookup_impl(key_t &key, std::vector<int> &groups);

	std::vector<lookup_result_t> write_impl(key_t &key, data_container_t &data, uint64_t offset, uint64_t size,
				uint64_t cflags, uint64_t ioflags, std::vector<int> &groups, int success_copies_num);

	data_container_t read_impl(key_t &key, uint64_t offset, uint64_t size,
				uint64_t cflags, uint64_t ioflags, std::vector<int> &groups,
				bool latest, bool embeded);

	void remove_impl(key_t &key, std::vector<int> &groups);

	std::vector<std::string> range_get_impl(key_t &from, key_t &to, uint64_t cflags, uint64_t ioflags,
				uint64_t limit_start, uint64_t limit_num, const std::vector<int> &groups, key_t &key);

	std::map<key_t, data_container_t> bulk_read_impl(std::vector<key_t> &keys, uint64_t cflags, std::vector<int> &groups);

		std::vector<elliptics_proxy_t::remote> lookup_addr_impl(key_t &key, std::vector<int> &groups);

	std::map<key_t, std::vector<lookup_result_t> > bulk_write_impl(std::vector<key_t> &keys, std::vector<data_container_t> &data, uint64_t cflags,
															  std::vector<int> &groups, int success_copies_num);

	std::string exec_script_impl(key_t &key, std::string &data, std::string &script, std::vector<int> &groups);

	async_read_result_t read_async_impl(key_t &key, uint64_t offset, uint64_t size,
									  uint64_t cflags, uint64_t ioflags, std::vector<int> &groups,
									  bool latest, bool embeded);

	async_write_result_t write_async_impl(key_t &key, data_container_t &data, uint64_t offset, uint64_t size,
										  uint64_t cflags, uint64_t ioflags, std::vector<int> &groups,
										  int success_copies_num);

	async_remove_result remove_async_impl(key_t &key, std::vector<int> &groups);

	bool ping();
	std::vector<status_result_t> stat_log();

	std::string id_str(const key_t &key);

	lookup_result_t parse_lookup(const ioremap::elliptics::lookup_result_entry &l);
	std::vector<lookup_result_t> parse_lookup(const std::vector<ioremap::elliptics::lookup_result_entry> &l);

	std::vector<int> get_groups(key_t &key, const std::vector<int> &groups, int count = 0) const;

	async_update_indexes_result_t update_indexes_async_impl(key_t &key, std::vector<std::string> &indexes, std::vector<ioremap::elliptics::data_pointer> &data);
	async_update_indexes_result_t update_indexes_async(const key_t &key, const std::vector<index_entry_t> &indexes);
	async_find_indexes_result_t find_indexes_async(const std::vector<dnet_raw_id> &indexes);
	async_find_indexes_result_t find_indexes_async(const std::vector<std::string> &indexes);
	async_check_indexes_result_t check_indexes_async(const key_t &key);


#ifdef HAVE_METABASE
	std::vector<int> get_metabalancer_groups_impl(uint64_t count, uint64_t size, key_t &key);
	group_info_response_t get_metabalancer_group_info_impl(int group);

	std::vector<std::vector<int> > get_symmetric_groups();
	std::map<int, std::vector<int> > get_bad_groups();
	std::vector<int> get_all_groups();

	bool collect_group_weights();
	void collect_group_weights_loop();
#endif /* HAVE_METABASE */

private:
	std::shared_ptr<ioremap::elliptics::file_logger>   m_elliptics_log;
	std::shared_ptr<ioremap::elliptics::node>          m_elliptics_node;
	std::vector<int>                                   m_groups;

	int                                                m_base_port;
	int                                                m_directory_bit_num;
	int                                                m_success_copies_num;
	int                                                m_die_limit;
	int                                                m_replication_count;
	int                                                m_chunk_size;
	bool                                               m_eblob_style_path;

#ifdef HAVE_METABASE
	std::unique_ptr<cocaine::dealer::dealer_t>         m_cocaine_dealer;
	cocaine::dealer::message_policy_t                  m_cocaine_default_policy;
	int                                                m_metabase_timeout;
	int                                                m_metabase_usage;
	uint64_t                                           m_metabase_current_stamp;

	std::unique_ptr<group_weights_cache_interface_t>   m_weight_cache;
	const int                                          m_group_weights_update_period;
	std::thread                                        m_weight_cache_update_thread;
	std::condition_variable                            m_weight_cache_condition_variable;
	std::mutex                                         m_mutex;
	bool                                               m_done;
#endif /* HAVE_METABASE */
};

// elliptics_proxy_t

elliptics_proxy_t::elliptics_proxy_t(const elliptics_proxy_t::config &c)
	: pimpl (new elliptics_proxy_t::impl(c)) {
}

elliptics_proxy_t::~elliptics_proxy_t() {
}

lookup_result_t elliptics_proxy_t::lookup_impl(key_t &key, std::vector<int> &groups) {
	return pimpl->lookup_impl(key, groups);
}

std::vector<lookup_result_t> elliptics_proxy_t::write_impl(key_t &key, data_container_t &data, uint64_t offset, uint64_t size,
			uint64_t cflags, uint64_t ioflags, std::vector<int> &groups, int success_copies_num) {
	return pimpl->write_impl(key, data, offset, size, cflags, ioflags, groups, success_copies_num);
}

data_container_t elliptics_proxy_t::read_impl(key_t &key, uint64_t offset, uint64_t size,
			uint64_t cflags, uint64_t ioflags, std::vector<int> &groups,
			bool latest, bool embeded) {
	return pimpl->read_impl(key, offset, size, cflags, ioflags, groups, latest, embeded);
}

void elliptics_proxy_t::remove_impl(key_t &key, std::vector<int> &groups) {
	pimpl->remove_impl(key, groups);
}

std::vector<std::string> elliptics_proxy_t::range_get_impl(key_t &from, key_t &to, uint64_t cflags, uint64_t ioflags,
			uint64_t limit_start, uint64_t limit_num, const std::vector<int> &groups, key_t &key) {
	return pimpl->range_get_impl(from, to, cflags, ioflags, limit_start, limit_num, groups, key);
}

std::map<key_t, data_container_t> elliptics_proxy_t::bulk_read_impl(std::vector<key_t> &keys, uint64_t cflags, std::vector<int> &groups) {
	return pimpl->bulk_read_impl(keys, cflags, groups);
}

std::vector<elliptics_proxy_t::remote> elliptics_proxy_t::lookup_addr_impl(key_t &key, std::vector<int> &groups) {
	return pimpl->lookup_addr_impl(key, groups);
}

std::map<key_t, std::vector<lookup_result_t> > elliptics_proxy_t::bulk_write_impl(std::vector<key_t> &keys, std::vector<data_container_t> &data, uint64_t cflags,
														  std::vector<int> &groups, int success_copies_num) {
	return pimpl->bulk_write_impl(keys, data, cflags, groups, success_copies_num);
}

std::string elliptics_proxy_t::exec_script_impl(key_t &key, std::string &data, std::string &script, std::vector<int> &groups) {
	return pimpl->exec_script_impl(key, data, script, groups);
}

async_read_result_t elliptics_proxy_t::read_async_impl(key_t &key, uint64_t offset, uint64_t size,
								  uint64_t cflags, uint64_t ioflags, std::vector<int> &groups,
								  bool latest, bool embeded) {
	return pimpl->read_async_impl(key, offset, size, cflags, ioflags, groups, latest, embeded);
}

async_write_result_t elliptics_proxy_t::write_async_impl(key_t &key, data_container_t &data, uint64_t offset, uint64_t size,
									  uint64_t cflags, uint64_t ioflags, std::vector<int> &groups,
									  int success_copies_num) {
	return pimpl->write_async_impl(key, data, offset, size, cflags, ioflags, groups, success_copies_num);
}

async_remove_result elliptics_proxy_t::remove_async_impl(key_t &key, std::vector<int> &groups) {
	return pimpl->remove_async_impl(key, groups);
}

std::vector<int> elliptics_proxy_t::get_groups(key_t &key, const std::vector<int> &groups, int count) const {
	return pimpl->get_groups(key, groups, count);
}

#ifdef HAVE_METABASE

std::vector<int> elliptics_proxy_t::get_metabalancer_groups_impl(uint64_t count, uint64_t size, key_t &key) {
	return pimpl->get_metabalancer_groups_impl(count, size, key);
}

group_info_response_t elliptics_proxy_t::get_metabalancer_group_info_impl(int group) {
	return pimpl->get_metabalancer_group_info_impl(group);
}

std::vector<std::vector<int> > elliptics_proxy_t::get_symmetric_groups() {
	return pimpl->get_symmetric_groups();
}

std::map<int, std::vector<int> > elliptics_proxy_t::get_bad_groups() {
	return pimpl->get_bad_groups();
}

std::vector<int> elliptics_proxy_t::get_all_groups() {
	return pimpl->get_all_groups();
}

#endif /* HAVE_METABASE */

bool elliptics_proxy_t::ping() {
	return pimpl->ping();
}

std::vector<status_result_t> elliptics_proxy_t::stat_log() {
	return pimpl->stat_log();
}

std::string elliptics_proxy_t::id_str(const key_t &key) {
	return pimpl->id_str(key);
}

async_update_indexes_result_t elliptics_proxy_t::update_indexes_async_impl(key_t &key, std::vector<std::string> &indexes, std::vector<ioremap::elliptics::data_pointer> &data) {
	return pimpl->update_indexes_async_impl(key, indexes, data);
}

async_update_indexes_result_t elliptics_proxy_t::update_indexes_async(const key_t &key, const std::vector<index_entry_t> &indexes) {
	return pimpl->update_indexes_async(key, indexes);
}

async_find_indexes_result_t elliptics_proxy_t::find_indexes_async(const std::vector<dnet_raw_id> &indexes) {
	return pimpl->find_indexes_async(indexes);
}

async_find_indexes_result_t elliptics_proxy_t::find_indexes_async(const std::vector<std::string> &indexes) {
	return pimpl->find_indexes_async(indexes);
}

async_check_indexes_result_t elliptics_proxy_t::check_indexes_async(const key_t &key) {
	return pimpl->check_indexes_async(key);
}

// pimpl

elliptics::elliptics_proxy_t::impl::impl(const elliptics_proxy_t::config &c) :
	m_groups(c.groups),
	m_base_port(c.base_port),
	m_directory_bit_num(c.directory_bit_num),
	m_success_copies_num(c.success_copies_num),
	m_die_limit(c.die_limit),
	m_replication_count(c.replication_count),
	m_chunk_size(c.chunk_size),
	m_eblob_style_path(c.eblob_style_path)
#ifdef HAVE_METABASE
	,m_cocaine_dealer()
	,m_metabase_usage(PROXY_META_NONE)
	,m_weight_cache(get_group_weighs_cache())
	,m_group_weights_update_period(c.group_weights_refresh_period)
	,m_done(false)
#endif /* HAVE_METABASE */
{
	if (m_replication_count == 0) {
		m_replication_count = m_groups.size();
	}
	if (m_success_copies_num == 0) {
		m_success_copies_num = SUCCESS_COPIES_TYPE__QUORUM;
	}
	if (!c.remotes.size()) {
		throw std::runtime_error("Remotes can't be empty");
	}

	struct dnet_config dnet_conf;
	memset(&dnet_conf, 0, sizeof (dnet_conf));

	dnet_conf.wait_timeout = c.wait_timeout;
	dnet_conf.check_timeout = c.check_timeout;
	dnet_conf.flags = c.flags;

	m_elliptics_log.reset(new ioremap::elliptics::file_logger(c.log_path.c_str(), c.log_mask));
	m_elliptics_node.reset(new ioremap::elliptics::node(*m_elliptics_log, dnet_conf));

	for (std::vector<elliptics_proxy_t::remote>::const_iterator it = c.remotes.begin(); it != c.remotes.end(); ++it) {
		try {
			m_elliptics_node->add_remote(it->host.c_str(), it->port, it->family);
		} catch(const std::exception &e) {
			std::stringstream msg;
			msg << "Can't connect to remote node " << it->host << ":" << it->port << ":" << it->family << " : " << e.what() << std::endl;
			m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		}
	}
#ifdef HAVE_METABASE
	if (c.cocaine_config.size()) {
		m_cocaine_dealer.reset(new cocaine::dealer::dealer_t(c.cocaine_config));
	}

	m_cocaine_default_policy.deadline = c.wait_timeout;
	if (m_cocaine_dealer.get()) {
		m_weight_cache_update_thread = std::thread(std::bind(&elliptics_proxy_t::impl::collect_group_weights_loop, this));
	}
#endif /* HAVE_METABASE */
}

elliptics::elliptics_proxy_t::impl::~impl() {
#ifdef HAVE_METABASE
	try {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			(void)lock;
			m_done = true;
			m_weight_cache_condition_variable.notify_one();
		}
		m_weight_cache_update_thread.join();
	} catch (std::system_error) {

	}

#endif /* HAVE_METABASE */
}

lookup_result_t parse_lookup(const ioremap::elliptics::lookup_result_entry &l, bool eblob_style_path, int base_port)
{
	return lookup_result_t(l, eblob_style_path, base_port);
}

std::vector<lookup_result_t> parse_lookup(const std::vector<lookup_result_entry> &l, bool eblob_style_path, int base_port)
{
	std::vector<lookup_result_t> ret;

	for (size_t i = 0; i < l.size(); ++i)
		ret.push_back(parse_lookup(l[i], eblob_style_path, base_port));

	return ret;
}

lookup_result_t elliptics_proxy_t::impl::parse_lookup(const ioremap::elliptics::lookup_result_entry &l)
{
	return elliptics::parse_lookup(l, m_eblob_style_path, m_base_port);
}

std::vector<lookup_result_t> elliptics_proxy_t::impl::parse_lookup(const std::vector<lookup_result_entry> &l)
{
	return elliptics::parse_lookup(l, m_eblob_style_path, m_base_port);
}

lookup_result_t elliptics_proxy_t::impl::lookup_impl(key_t &key, std::vector<int> &groups)
{
	session elliptics_session(*m_elliptics_node);
	elliptics_session.set_filter(ioremap::elliptics::filters::all);
	std::vector<int> lgroups;

	lgroups = get_groups(key, groups);

	try {
		while (!lgroups.empty()) {
			elliptics_session.set_groups(lgroups);
			sync_lookup_result result = elliptics_session.lookup(key).get();
			std::vector<int>::iterator end = lgroups.end();
			for (auto it = result.begin(); it != result.end(); ++it) {
				lookup_result_entry &entry = *it;
				if (!entry.error()) {
					try {
						return parse_lookup(entry);
					} catch (...) {
					}
				}
				end = std::remove(lgroups.begin(), end, entry.command()->id.group_id);
			}
			lgroups.erase(end, lgroups.end());
		}
		throw ioremap::elliptics::not_found_error(key.to_string());
	} catch (...) {
		std::stringstream msg;
		msg << "can not get download info for key " << key.to_string() << std::endl;
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
}

data_container_t elliptics_proxy_t::impl::read_impl(key_t &key, uint64_t offset, uint64_t size,
				uint64_t cflags, uint64_t ioflags, std::vector<int> &groups,
				bool latest, bool embeded)
{
	return read_async_impl(key, offset, size, cflags, ioflags, groups, latest, embeded).get_one();
}

std::vector<lookup_result_t> elliptics_proxy_t::impl::write_impl(key_t &key, data_container_t &data, uint64_t offset, uint64_t size,
					uint64_t cflags, uint64_t ioflags, std::vector<int> &groups, int success_copies_num)
{
	unsigned int replication_count = groups.size();
	session elliptics_session(*m_elliptics_node);
	bool use_metabase = false;

	elliptics_session.set_cflags(cflags);
	elliptics_session.set_ioflags(ioflags);

	if (elliptics_session.state_num() < m_die_limit) {
		throw std::runtime_error("Too low number of existing states");
	}

	if (replication_count == 0) {
		replication_count = m_replication_count;
	}
	if (success_copies_num == 0) {
		success_copies_num = m_success_copies_num;
	}

	std::vector<int> lgroups = get_groups(key, groups);
#ifdef HAVE_METABASE
	if (m_metabase_usage >= PROXY_META_OPTIONAL) {
		try {
			if (groups.size() != replication_count || m_metabase_usage == PROXY_META_MANDATORY) {
				std::vector<int> mgroups = get_metabalancer_groups_impl(replication_count, size, key);
				lgroups = mgroups;
			}
			use_metabase = 1;
		} catch (std::exception &e) {
			m_elliptics_log->log(DNET_LOG_ERROR, e.what());
			if (m_metabase_usage >= PROXY_META_NORMAL) {
				throw std::runtime_error("Metabase does not respond");
			}
		}
	}
#endif /* HAVE_METABASE */
	if (replication_count != 0 && (size_t)replication_count < lgroups.size())
		lgroups.erase(lgroups.begin() + replication_count, lgroups.end());

	write_helper_t helper(success_copies_num, replication_count, lgroups);

	try {
		elliptics_session.set_groups(lgroups);

		bool chunked = false;

		ioremap::elliptics::data_pointer content = data_container_t::pack(data);

		if (m_chunk_size && content.size() > static_cast<size_t>(m_chunk_size) && !key.by_id()
				&& !(ioflags & (DNET_IO_FLAGS_PREPARE | DNET_IO_FLAGS_COMMIT | DNET_IO_FLAGS_PLAIN_WRITE))) {
			chunked = true;
		}

		std::vector<write_result_entry> lookup;

		try {
			if (ioflags & DNET_IO_FLAGS_PREPARE) {
				lookup = elliptics_session.write_prepare(key, content, offset, size).get();
			} else if (ioflags & DNET_IO_FLAGS_COMMIT) {
				lookup = elliptics_session.write_commit(key, content, offset, size).get();
			} else if (ioflags & DNET_IO_FLAGS_PLAIN_WRITE) {
				lookup = elliptics_session.write_plain(key, content, offset).get();
			} else {
				if (chunked) {
					ioremap::elliptics::data_pointer write_content;
					bool last_iter = false;

					write_content = content.slice(offset, m_chunk_size);
					lookup = elliptics_session.write_prepare(key, write_content, offset, content.size()).get();
					helper.update_lookup(parse_lookup(lookup), false);

					if (helper.upload_is_good()) {
						do {
							elliptics_session.set_groups(helper.get_upload_groups());
							offset += m_chunk_size;

							if (offset + m_chunk_size >= content.size()) {
								write_content = content.slice(offset, content.size() - offset);
								lookup = elliptics_session.write_commit(key, write_content, offset, content.size()).get();
								last_iter = true;
							}
							else {
								write_content = content.slice(offset, m_chunk_size);
								lookup = elliptics_session.write_plain(key, write_content, offset).get();
							}
							helper.update_lookup(parse_lookup(lookup), last_iter);
						} while (helper.upload_is_good() && (offset + m_chunk_size < content.size()));
					}

				} else {
					lookup = elliptics_session.write_data(key, content, offset).get();
				}
			}

			if (!chunked)
				helper.update_lookup(parse_lookup(lookup));

			if (!helper.upload_is_good()) {
				elliptics_session.set_groups(lgroups);
				elliptics_session.set_filter(ioremap::elliptics::filters::all);
				elliptics_session.remove(key.remote());
				throw std::runtime_error("Not enough copies was written, or problems with chunked upload");
			}

			if (chunked && helper.has_incomplete_groups()) {
				elliptics_session.set_groups(helper.get_incomplete_groups());
				elliptics_session.set_filter(ioremap::elliptics::filters::all);
				elliptics_session.remove(key.remote());
			}

		}
		catch (const std::exception &e) {
			std::stringstream msg;
			msg << "Can't write data for key " << key.to_string() << " " << e.what();
			m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
			throw;
		}
		catch (...) {
			m_elliptics_log->log(DNET_LOG_ERROR, "Can't write data for key: unknown");
			throw;
		}

		struct timespec ts;
		memset(&ts, 0, sizeof(ts));

		elliptics_session.set_cflags(0);
		elliptics_session.write_metadata(key, key.remote(), helper.get_upload_groups(), ts);
		elliptics_session.set_cflags(ioflags);
	}
	catch (const std::exception &e) {
		std::stringstream msg;
		msg << "Can't write data for key " << key.to_string() << " " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
	catch (...) {
		std::stringstream msg;
		msg << "Can't write data for key " << key.to_string();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}

	return helper.get_result();
}

std::vector<std::string> elliptics_proxy_t::impl::range_get_impl(key_t &from, key_t &to, uint64_t cflags, uint64_t ioflags,
					uint64_t limit_start, uint64_t limit_num, const std::vector<int> &groups, key_t &key)
{
	session elliptics_session(*m_elliptics_node);
	std::vector<int> lgroups;
	lgroups = get_groups(key, groups);

	elliptics_session.set_cflags(cflags);
	elliptics_session.set_ioflags(ioflags);

	std::vector<std::string> ret;

	try {
		struct dnet_io_attr io;
		memset(&io, 0, sizeof(struct dnet_io_attr));

		if (from.by_id()) {
			memcpy(io.id, from.id().id, sizeof(io.id));
		}

		if (to.by_id()) {
			memcpy(io.parent, from.id().id, sizeof(io.parent));
		} else {
			memset(io.parent, 0xff, sizeof(io.parent));
		}

		io.start = limit_start;
		io.num = limit_num;
		io.flags = ioflags;
		io.type = from.type();


		for (size_t i = 0; i < lgroups.size(); ++i) {
			try {
				sync_read_result range_result = elliptics_session.read_data_range(io, lgroups[i]).get();

				uint64_t num = 0;

				for (size_t i = 0; i < range_result.size(); ++i) {
					read_result_entry entry = range_result[i];
					if (!(io.flags & DNET_IO_FLAGS_NODATA))
						num += entry.io_attribute()->num;
					else
						ret.push_back(entry.data().to_string());
				}

				if (io.flags & DNET_IO_FLAGS_NODATA) {
					std::ostringstream str;
					str << num;
					ret.push_back(str.str());
				}
				if (ret.size())
					break;
			} catch (...) {
				continue;
			}
		}

		if (ret.size() == 0) {
			std::ostringstream str;
			str << "READ_RANGE failed for key " << key.to_string() << " in " << groups.size() << " groups";
			throw std::runtime_error(str.str());
		}


	}
	catch (const std::exception &e) {
		std::stringstream msg;
		msg << "READ_RANGE failed for key " << key.to_string() << " from:" << from.to_string() << " to:" << to.to_string() << " " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
	catch (...) {
		std::stringstream msg;
		msg << "READ_RANGE failed for key " << key.to_string() << " from:" << from.to_string() << " to:" << to.to_string();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}

	return ret;
}

void elliptics_proxy_t::impl::remove_impl(key_t &key, std::vector<int> &groups)
{
	remove_async_impl(key, groups).wait();
}

std::map<key_t, data_container_t> elliptics_proxy_t::impl::bulk_read_impl(std::vector<key_t> &keys, uint64_t cflags, std::vector<int> &groups)
{
	std::map<key_t, data_container_t> ret;

	if (!keys.size())
		return ret;

	session elliptics_session(*m_elliptics_node);
	std::vector<int> lgroups = get_groups(keys[0], groups);

	std::map<struct dnet_id, key_t, dnet_id_less> keys_transformed;

	try {
		elliptics_session.set_groups(lgroups);

		std::vector<struct dnet_io_attr> ios;
		ios.reserve(keys.size());

		for (std::vector<key_t>::iterator it = keys.begin(); it != keys.end(); it++) {
			struct dnet_io_attr io;
			memset(&io, 0, sizeof(io));

			key_t tmp(*it);
			if (!tmp.by_id()) {

				tmp.transform(elliptics_session);
			}


			memcpy(io.id, tmp.id().id, sizeof(io.id));
			ios.push_back(io);
			keys_transformed.insert(std::make_pair(tmp.id(), *it));
		}

		auto result = elliptics_session.bulk_read(ios).get();

		for (auto it = result.begin(), end = result.end(); it != end; ++it) {
			read_result_entry &entry = *it;

			ret.insert(std::make_pair(keys_transformed[entry.command()->id], data_container_t::unpack(entry.file())));
		}


	}
	catch (const std::exception &e) {
		std::stringstream msg;
		msg << "can not bulk get data " << e.what() << std::endl;
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
	catch (...) {
		std::stringstream msg;
		msg << "can not bulk get data" << std::endl;
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}

	return ret;
}

std::vector<elliptics_proxy_t::remote> elliptics_proxy_t::impl::lookup_addr_impl(key_t &key, std::vector<int> &groups)
{
	session elliptics_session(*m_elliptics_node);
	std::vector<int> lgroups = get_groups(key, groups);

	std::vector<elliptics_proxy_t::remote> addrs;

	for (std::vector<int>::const_iterator it = groups.begin();
			it != groups.end(); it++)
	{
		std::string ret;

		if (key.by_id()) {
			struct dnet_id id = key.id();
		ret = elliptics_session.lookup_address(id, *it);

		} else {
		ret = elliptics_session.lookup_address(key.remote(), *it);
		}

		size_t pos = ret.find(':');
		elliptics_proxy_t::remote addr(ret.substr(0, pos), boost::lexical_cast<int>(ret.substr(pos+1, std::string::npos)));

		addrs.push_back(addr);
	}

	return addrs;
}

std::map<key_t, std::vector<lookup_result_t> > elliptics_proxy_t::impl::bulk_write_impl(std::vector<key_t> &keys, std::vector<data_container_t> &data, uint64_t cflags,
																		   std::vector<int> &groups, int success_copies_num) {
	unsigned int replication_count = groups.size();
	std::map<key_t, std::vector<lookup_result_t> > res;
	std::map<key_t, std::vector<int> > res_groups;

	if (!keys.size())
		return res;

	session elliptics_session(*m_elliptics_node);

	if (replication_count == 0)
		replication_count = m_replication_count;

	std::vector<int> lgroups = get_groups(keys[0], groups);

	std::map<struct dnet_id, key_t, dnet_id_less> keys_transformed;

	try {
		if (keys.size() != data.size())
			throw std::runtime_error("counts of keys and data are not equal");

		elliptics_session.set_groups(lgroups);

		std::vector<struct dnet_io_attr> ios;
		std::vector<ioremap::elliptics::data_pointer> data_pointers;
		ios.reserve(keys.size());
		data_pointers.reserve(keys.size());

		for (size_t index = 0; index != keys.size(); ++index) {
			data_pointers.push_back(data_container_t::pack(data[index]));

			struct dnet_io_attr io;
			memset(&io, 0, sizeof(io));

			key_t tmp(keys [index]);
			if (!tmp.by_id()) {
				tmp.transform(elliptics_session);
			}

			memcpy(io.id, tmp.id().id, sizeof(io.id));
			io.size = data_pointers[index].size();
			ios.push_back(io);
			keys_transformed.insert(std::make_pair(tmp.id(), keys [index]));
		}

		 auto result = elliptics_session.bulk_write(ios, data_pointers).get();

		 //for (size_t i = 0; i != result.size(); ++i) {
		 for (auto it = result.begin(), end = result.end(); it != end; ++it) {
			 const ioremap::elliptics::lookup_result_entry &lr = *it;//result [i];
			 lookup_result_t r = parse_lookup(lr);
			 //ID ell_id(lr->command()->id);
			 key_t key = keys_transformed [lr.command()->id];
			 res [key].push_back(r);
			 res_groups [key].push_back(r.group());
		 }

		 unsigned int replication_need =  uploads_need(success_copies_num != 0 ? success_copies_num : m_success_copies_num,
													   replication_count);

		 auto it = res_groups.begin();
		 auto end = res_groups.end();
		 for (; it != end; ++it) {
			 if (it->second.size() < replication_need)
				 break;
		 }

		 if (it != end) {
			 for (auto it = res_groups.begin(), end = res_groups.end(); it != end; ++it) {
				 elliptics_session.set_groups(it->second);
				 elliptics_session.remove(it->first.remote());
			 }
			 throw std::runtime_error("Not enough copies was written");
		 }

	}
	catch (const std::exception &e) {
		std::stringstream msg;
		msg << "can not bulk write data " << e.what() << std::endl;
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
	catch (...) {
		std::stringstream msg;
		msg << "can not bulk write data" << std::endl;
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}

	return res;
}

std::string elliptics_proxy_t::impl::exec_script_impl(key_t &key, std::string &data, std::string &script, std::vector<int> &groups) {
	std::string res;
	ioremap::elliptics::session sess(*m_elliptics_node);
	if (sess.state_num() < m_die_limit) {
		throw std::runtime_error("Too low number of existing states");
	}

	struct dnet_id id;
	memset(&id, 0, sizeof(id));

	if (key.by_id()) {
		id = key.id();
	} else {
		sess.transform(key.remote(), id);
		id.type = key.type();
	}

	std::vector<int> lgroups = get_groups(key, groups);

	try {
		sess.set_groups(lgroups);
		res = sess.exec_locked(&id, script, data, std::string());
	}
	catch (const std::exception &e) {
		std::stringstream msg;
		msg << "can not execute script  " << script << "; " << e.what() << std::endl;
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
	catch (...) {
		std::stringstream msg;
		msg << "can not execute script  " << script << std::endl;
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
	return res;
}

async_read_result_t elliptics_proxy_t::impl::read_async_impl(key_t &key, uint64_t offset, uint64_t size,
												  uint64_t cflags, uint64_t ioflags, std::vector<int> &groups,
												  bool latest, bool embeded) {
	session elliptics_session(*m_elliptics_node);
	std::vector<int> lgroups;
	lgroups = get_groups(key, groups);

	elliptics_session.set_cflags(cflags);
	elliptics_session.set_ioflags(ioflags);

	try {
		elliptics_session.set_groups(lgroups);

		if (latest)
			return async_read_result_t(elliptics_session.read_latest(key, offset, size), embeded);
		else
			return async_read_result_t(elliptics_session.read_data(key, offset, size), embeded);
	}
	catch (const std::exception &e) {
		std::stringstream msg;
		msg << "can not get data for key " << key.to_string() << " " << e.what() << std::endl;
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
	catch (...) {
		std::stringstream msg;
		msg << "can not get data for key " << key.to_string() << std::endl;
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
}

async_write_result_t elliptics_proxy_t::impl::write_async_impl(key_t &key, data_container_t &data, uint64_t offset, uint64_t size,
													  uint64_t cflags, uint64_t ioflags, std::vector<int> &groups,
													  int success_copies_num)
{
	unsigned int replication_count = groups.size();
	session elliptics_session(*m_elliptics_node);
	bool use_metabase = false;

	elliptics_session.set_cflags(cflags);
	elliptics_session.set_ioflags(ioflags);

	if (elliptics_session.state_num() < m_die_limit) {
		throw std::runtime_error("Too low number of existing states");
	}

	if (replication_count == 0) {
		replication_count = m_replication_count;
	}

	std::vector<int> lgroups = get_groups(key, groups);
#ifdef HAVE_METABASE
	if (m_metabase_usage >= PROXY_META_OPTIONAL) {
		try {
			if (groups.size() != replication_count || m_metabase_usage == PROXY_META_MANDATORY) {
				std::vector<int> mgroups = get_metabalancer_groups_impl(replication_count, size, key);
				lgroups = mgroups;
			}
			use_metabase = 1;
		} catch (std::exception &e) {
			m_elliptics_log->log(DNET_LOG_ERROR, e.what());
			if (m_metabase_usage >= PROXY_META_NORMAL) {
				throw std::runtime_error("Metabase does not respond");
			}
		}
	}
#endif /* HAVE_METABASE */
	if (replication_count != 0 && (size_t)replication_count < lgroups.size())
		lgroups.erase(lgroups.begin() + replication_count, lgroups.end());



	try {
		elliptics_session.set_groups(lgroups);

		ioremap::elliptics::data_pointer content = data_container_t::pack(data);

		if (ioflags & DNET_IO_FLAGS_PREPARE) {
			return async_write_result_t(elliptics_session.write_prepare(key, content, offset, size), m_eblob_style_path, m_base_port);
		} else if (ioflags & DNET_IO_FLAGS_COMMIT) {
			return async_write_result_t(elliptics_session.write_commit(key, content, offset, size), m_eblob_style_path, m_base_port);
		} else if (ioflags & DNET_IO_FLAGS_PLAIN_WRITE) {
			return async_write_result_t(elliptics_session.write_plain(key, content, offset), m_eblob_style_path, m_base_port);
		} else {
			return async_write_result_t(elliptics_session.write_data(key, content, offset), m_eblob_style_path, m_base_port);
		}
	}
	catch (const std::exception &e) {
		std::stringstream msg;
		msg << "Can't write data for key " << key.to_string() << " " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
	catch (...) {
		std::stringstream msg;
		msg << "Can't write data for key " << key.to_string();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
}

async_remove_result_t elliptics_proxy_t::impl::remove_async_impl(key_t &key, std::vector<int> &groups) {
	session elliptics_session(*m_elliptics_node);
	std::vector<int> lgroups;

	lgroups = get_groups(key, groups);
	try {
		elliptics_session.set_groups(lgroups);
		elliptics_session.set_filter(ioremap::elliptics::filters::all);
		return elliptics_session.remove(key);
	}
	catch (const std::exception &e) {
		std::stringstream msg;
		msg << "Can't remove object " << key.to_string() << " " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
	catch (...) {
		std::stringstream msg;
		msg << "Can't remove object " << key.to_string();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
}

bool elliptics_proxy_t::impl::ping() {
	ioremap::elliptics::session sess(*m_elliptics_node);
	return sess.state_num() >= m_die_limit;
}

std::vector<status_result_t> elliptics_proxy_t::impl::stat_log() {
	std::vector<status_result_t> res;

	ioremap::elliptics::session sess(*m_elliptics_node);

	std::vector<ioremap::elliptics::stat_result_entry> srs = sess.stat_log().get();

	char id_str[DNET_ID_SIZE * 2 + 1];
	char addr_str[128];

	status_result_t sr;

	//for (size_t i = 0; i < srs.size(); ++i) {
	for (auto it = srs.begin(), end = srs.end(); it != end; ++it) {
		const ioremap::elliptics::stat_result_entry &data = *it;//srs[i];
		struct dnet_addr *addr = data.address();
		struct dnet_cmd *cmd = data.command();
		struct dnet_stat *st = data.statistics();

		dnet_server_convert_dnet_addr_raw(addr, addr_str, sizeof(addr_str));
		dnet_dump_id_len_raw(cmd->id.id, DNET_ID_SIZE, id_str);

		sr.la[0] = (float)st->la[0] / 100.0;
		sr.la[1] = (float)st->la[1] / 100.0;
		sr.la[2] = (float)st->la[2] / 100.0;

		sr.addr.assign(addr_str);
		sr.id.assign(id_str);
		sr.vm_total = st->vm_total;
		sr.vm_free = st->vm_free;
		sr.vm_cached = st->vm_cached;
		sr.storage_size = st->frsize * st->blocks / 1024 / 1024;
		sr.available_size = st->bavail * st->bsize / 1024 / 1024;
		sr.files = st->files;
		sr.fsid = st->fsid;

		res.push_back(sr);
	}

	return res;
}

std::string elliptics_proxy_t::impl::id_str(const key_t &key) {
	ioremap::elliptics::session sess(*m_elliptics_node);
	struct dnet_id id;
	memset(&id, 0, sizeof(id));
	if (key.by_id()) {
		id = key.id();
	} else {
		sess.transform(key.remote(), id);
	}
	char str[2 * DNET_ID_SIZE + 1];
	dnet_dump_id_len_raw(id.id, DNET_ID_SIZE, str);
	return std::string(str);
}

std::vector<int> elliptics_proxy_t::impl::get_groups(key_t &key, const std::vector<int> &groups, int count) const {
	std::vector<int> lgroups;

	if (groups.size()) {
		lgroups = groups;
	}
	else {
		lgroups = m_groups;
		if (lgroups.size() > 1) {
			std::vector<int>::iterator git = lgroups.begin();
			++git;
			std::random_shuffle(git, lgroups.end());
		}
	}

	if (count != 0 && count < (int)(lgroups.size())) {
		lgroups.erase(lgroups.begin() + count, lgroups.end());
	}

	if (!lgroups.size()) {
		throw std::runtime_error("There is no groups");
	}

	return lgroups;
}

async_update_indexes_result_t elliptics_proxy_t::impl::update_indexes_async_impl(key_t &key, std::vector<std::string> &indexes, std::vector<ioremap::elliptics::data_pointer> &data) {
	ioremap::elliptics::session sess(*m_elliptics_node);
	if (data.empty()) {
		data.resize(indexes.size());
	}
	return sess.update_indexes(key, indexes, data);
}

async_update_indexes_result_t elliptics_proxy_t::impl::update_indexes_async(const key_t &key, const std::vector<index_entry_t> &indexes) {
	ioremap::elliptics::session sess(*m_elliptics_node);
	return sess.update_indexes(key, indexes);
}

async_find_indexes_result_t elliptics_proxy_t::impl::find_indexes_async(const std::vector<dnet_raw_id> &indexes) {
	ioremap::elliptics::session sess(*m_elliptics_node);
	return sess.find_indexes(indexes);
}

async_find_indexes_result_t elliptics_proxy_t::impl::find_indexes_async(const std::vector<std::string> &indexes) {
	ioremap::elliptics::session sess(*m_elliptics_node);
	return sess.find_indexes(indexes);
}

async_check_indexes_result_t elliptics_proxy_t::impl::check_indexes_async(const key_t &key) {
	ioremap::elliptics::session sess(*m_elliptics_node);
	return sess.check_indexes(key);
}

#ifdef HAVE_METABASE
bool elliptics_proxy_t::impl::collect_group_weights()
{
	if (!m_cocaine_dealer.get()) {
		throw std::runtime_error("Dealer is not initialized");
	}

	metabase_group_weights_request_t req;
	metabase_group_weights_response_t resp;

	req.stamp = ++m_metabase_current_stamp;

	cocaine::dealer::message_path_t path("mastermind", "get_group_weights");

	boost::shared_ptr<cocaine::dealer::response_t> future;
	future = m_cocaine_dealer->send_message(req, path, m_cocaine_default_policy);

	cocaine::dealer::data_container chunk;
	future->get(&chunk);

	msgpack::unpacked unpacked;
	msgpack::unpack(&unpacked, static_cast<const char*>(chunk.data()), chunk.size());

	unpacked.get().convert(&resp);

	return m_weight_cache->update(resp);
}

void elliptics_proxy_t::impl::collect_group_weights_loop()
{
	std::unique_lock<std::mutex> lock(m_mutex);
#if __GNUC_MINOR__ >= 6
	auto no_timeout = std::cv_status::no_timeout;
	auto timeout = std::cv_status::timeout;
	auto tm = timeout;
#else
	bool no_timeout = false;
	bool timeout = true;
	bool tm = timeout;
#endif
	do {
		try {
			collect_group_weights();
			m_elliptics_log->log(DNET_LOG_INFO, "Updated group weights");
		} catch (const msgpack::unpack_error &e) {
			std::stringstream msg;
			msg << "Error while unpacking message: " << e.what();
			m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		} catch (const cocaine::dealer::dealer_error &e) {
			std::stringstream msg;
			msg << "Cocaine dealer error: " << e.what();
			m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		} catch (const cocaine::dealer::internal_error &e) {
			std::stringstream msg;
			msg << "Cocaine internal error: " << e.what();
			m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		} catch (const std::exception &e) {
			std::stringstream msg;
			msg << "Error while updating cache: " << e.what();
			m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		}
		tm = timeout;
		while(m_done == false)
			tm = m_weight_cache_condition_variable.wait_for(lock,
															std::chrono::seconds(
																m_group_weights_update_period));
	} while(no_timeout == tm);
}

std::vector<int> elliptics_proxy_t::impl::get_metabalancer_groups_impl(uint64_t count, uint64_t size, key_t &key)
{
	try {
		if(!m_weight_cache->initialized() && !collect_group_weights()) {
			return std::vector<int>();
		}
		std::vector<int> result = m_weight_cache->choose(count);

		std::ostringstream msg;

		msg << "Chosen group: [";

		std::vector<int>::const_iterator e = result.end();
		for(
				std::vector<int>::const_iterator it = result.begin();
				it != e;
				++it) {
			if(it != result.begin()) {
				msg << ", ";
			}
			msg << *it;
		}
		msg << "]\n";
		m_elliptics_log->log(DNET_LOG_INFO, msg.str().c_str());
		return result;

	} catch (const msgpack::unpack_error &e) {
		std::stringstream msg;
		msg << "Error while unpacking message: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	} catch (const cocaine::dealer::dealer_error &e) {
		std::stringstream msg;
		msg << "Cocaine dealer error: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	} catch (const cocaine::dealer::internal_error &e) {
		std::stringstream msg;
		msg << "Cocaine internal error: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
}

group_info_response_t elliptics_proxy_t::impl::get_metabalancer_group_info_impl(int group)
{
	if (!m_cocaine_dealer.get()) {
		throw std::runtime_error("Dealer is not initialized");
	}


	group_info_request_t req;
	group_info_response_t resp;

	req.group = group;

	try {
		cocaine::dealer::message_path_t path("mastermind", "get_group_info");

		boost::shared_ptr<cocaine::dealer::response_t> future;
		future = m_cocaine_dealer->send_message(req.group, path, m_cocaine_default_policy);

		cocaine::dealer::data_container chunk;
		future->get(&chunk);

		msgpack::unpacked unpacked;
		msgpack::unpack(&unpacked, static_cast<const char*>(chunk.data()), chunk.size());

		unpacked.get().convert(&resp);

	} catch (const msgpack::unpack_error &e) {
		std::stringstream msg;
		msg << "Error while unpacking message: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	} catch (const cocaine::dealer::dealer_error &e) {
		std::stringstream msg;
		msg << "Cocaine dealer error: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	} catch (const cocaine::dealer::internal_error &e) {
		std::stringstream msg;
		msg << "Cocaine internal error: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
		

	return resp;
}

std::vector<std::vector<int> > elliptics_proxy_t::impl::get_symmetric_groups() {
	std::vector<std::vector<int> > res;
	try {
		cocaine::dealer::message_path_t path("mastermind", "get_symmetric_groups");

		boost::shared_ptr<cocaine::dealer::response_t> future;
		future = m_cocaine_dealer->send_message(std::string(), path, m_cocaine_default_policy);

		cocaine::dealer::data_container chunk;
		future->get(&chunk);

		msgpack::unpacked unpacked;
		msgpack::unpack(&unpacked, static_cast<const char*>(chunk.data()), chunk.size());

		unpacked.get().convert(&res);
		return res;

	} catch (const msgpack::unpack_error &e) {
		std::stringstream msg;
		msg << "Error while unpacking message: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	} catch (const cocaine::dealer::dealer_error &e) {
		std::stringstream msg;
		msg << "Cocaine dealer error: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	} catch (const cocaine::dealer::internal_error &e) {
		std::stringstream msg;
		msg << "Cocaine internal error: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
}

std::map<int, std::vector<int> > elliptics_proxy_t::impl::get_bad_groups() {
	std::map<int, std::vector<int> > res;
	try {
		cocaine::dealer::message_path_t path("mastermind", "get_bad_groups");

		boost::shared_ptr<cocaine::dealer::response_t> future;
		future = m_cocaine_dealer->send_message(std::string(), path, m_cocaine_default_policy);

		cocaine::dealer::data_container chunk;
		future->get(&chunk);

		msgpack::unpacked unpacked;
		msgpack::unpack(&unpacked, static_cast<const char*>(chunk.data()), chunk.size());

		//std::vector<std::vector<int> > r;
		unpacked.get().convert(&res);
		return res;

	} catch (const msgpack::unpack_error &e) {
		std::stringstream msg;
		msg << "Error while unpacking message: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	} catch (const cocaine::dealer::dealer_error &e) {
		std::stringstream msg;
		msg << "Cocaine dealer error: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	} catch (const cocaine::dealer::internal_error &e) {
		std::stringstream msg;
		msg << "Cocaine internal error: " << e.what();
		m_elliptics_log->log(DNET_LOG_ERROR, msg.str().c_str());
		throw;
	}
}

std::vector<int> elliptics_proxy_t::impl::get_all_groups() {
	std::vector<int> res;

	{
		std::vector<std::vector<int> > r1 = get_symmetric_groups();
		for (auto it = r1.begin(); it != r1.end(); ++it) {
			res.insert(res.end(), it->begin(), it->end());
		}
	}

	{
		std::map<int, std::vector<int> > r2 = get_bad_groups();
		for (auto it = r2.begin(); it != r2.end(); ++it) {
			res.insert(res.end(), it->second.begin(), it->second.end());
		}
	}

	std::sort(res.begin(), res.end());
	res.erase(std::unique(res.begin(), res.end()), res.end());

	return res;
}
#endif /* HAVE_METABASE */

} // namespace elliptics

