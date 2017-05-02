#include "mysql_engine/handler.h"
#include "mysql_engine/plugin.h"

#include "pinba/globals.h"
#include "pinba/histogram.h"
// #include "pinba/packet.h"
#include "pinba/report_by_request.h"
#include "pinba/report_by_timer.h"
#include "pinba/report_by_packet.h"

// FIXME: some of these headers were moved to pinba_view, reassess,
//        or maybe move vews back here! since they're sql related anyway
#ifdef PINBA_USE_MYSQL_SOURCE
#include <sql/field.h> // <mysql/private/field.h>
#include <sql/handler.h> // <mysql/private/handler.h>
#include <include/mysqld_error.h> // <mysql/mysqld_error.h>
#else
#include <mysql/private/field.h>
#include <mysql/private/handler.h>
#include <mysql/mysqld_error.h>
#endif // PINBA_USE_MYSQL_SOURCE

#include <meow/defer.hpp>
#include <meow/stopwatch.hpp>

////////////////////////////////////////////////////////////////////////////////////////////////

#define STORE_FIELD(N, value)     \
	case N:                       \
		(*field)->set_notnull();  \
		(*field)->store(value);   \
	break;
/**/


struct pinba_view___base_t : public pinba_view_t
{
	virtual int  rnd_init(pinba_handler_t*, bool scan) override
	{
		return 0;
	}

	virtual int  rnd_end(pinba_handler_t*) override
	{
		return 0;
	}

	virtual int  rnd_next(pinba_handler_t*, uchar *buf) override
	{
		return HA_ERR_END_OF_FILE;
	}

	virtual int  rnd_pos(pinba_handler_t*, uchar *buf, uchar *pos) const override
	{
		return 0;
	}

	virtual void position(pinba_handler_t*, const uchar *record) const override
	{
		return;
	}

	virtual int  info(pinba_handler_t*, uint) const override
	{
		return 0;
	}

	virtual int  extra(pinba_handler_t*, enum ha_extra_function operation) const override
	{
		return 0;
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////

struct pinba_view___stats_t : public pinba_view___base_t
{
	pinba_status_variables_ptr vars_;

	virtual int  rnd_init(pinba_handler_t*, bool scan) override
	{
		vars_ = pinba_collect_status_variables();
		return 0;
	}

	virtual int  rnd_end(pinba_handler_t*) override
	{
		vars_.reset();
		return 0;
	}

	virtual int  rnd_next(pinba_handler_t *handler, uchar *buf) override
	{
		if (!vars_)
			return HA_ERR_END_OF_FILE;

		MEOW_DEFER(
			vars_.reset();
		);

		auto *table = handler->current_table();

		// mark all fields as writeable to avoid assert() in ::store() calls
		// got no idea how to do this properly anyway
		auto *old_map = tmp_use_all_columns(table, table->write_set);
		MEOW_DEFER(
			tmp_restore_column_map(table->write_set, old_map);
		);

		for (Field **field = table->field; *field; field++)
		{
			auto const field_index = (*field)->field_index;

			if (!bitmap_is_set(table->read_set, field_index))
				continue;

			switch(field_index)
			{
				STORE_FIELD(0,  vars_->uptime);

				STORE_FIELD(1,  vars_->udp_poll_total);
				STORE_FIELD(2,  vars_->udp_recv_total);
				STORE_FIELD(3,  vars_->udp_recv_eagain);
				STORE_FIELD(4,  vars_->udp_recv_bytes);
				STORE_FIELD(5,  vars_->udp_recv_packets);
				STORE_FIELD(6,  vars_->udp_packet_decode_err);
				STORE_FIELD(7,  vars_->udp_batch_send_total);
				STORE_FIELD(8,  vars_->udp_batch_send_err);
				STORE_FIELD(9,  vars_->udp_ru_utime);
				STORE_FIELD(10, vars_->udp_ru_stime);

				STORE_FIELD(11, vars_->repacker_poll_total);
				STORE_FIELD(12, vars_->repacker_recv_total);
				STORE_FIELD(13, vars_->repacker_recv_eagain);
				STORE_FIELD(14, vars_->repacker_recv_packets);
				STORE_FIELD(15, vars_->repacker_packet_validate_err);
				STORE_FIELD(16, vars_->repacker_batch_send_total);
				STORE_FIELD(17, vars_->repacker_batch_send_by_timer);
				STORE_FIELD(18, vars_->repacker_batch_send_by_size);
				STORE_FIELD(19, vars_->repacker_ru_utime);
				STORE_FIELD(20, vars_->repacker_ru_stime);

				STORE_FIELD(21, vars_->coordinator_batches_received);
				STORE_FIELD(22, vars_->coordinator_batch_send_total);
				STORE_FIELD(23, vars_->coordinator_batch_send_err);
				STORE_FIELD(24, vars_->coordinator_control_requests);
				STORE_FIELD(25, vars_->coordinator_ru_utime);
				STORE_FIELD(26, vars_->coordinator_ru_stime);

				STORE_FIELD(27, vars_->dictionary_size);
				STORE_FIELD(28, vars_->dictionary_mem_used);

			default:
				break;
			}
		}

		return 0;
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////

struct pinba_view___active_reports_t : public pinba_view___base_t
{
	struct view_row_t
	{
		pinba_share_data_t  share_data;
		report_state_ptr    report_state;
	};
	using view_t = std::vector<view_row_t>;

	view_t            data_;
	view_t::iterator  pos_;

	virtual int rnd_init(pinba_handler_t *handler, bool scan) override
	{
		// TODO:
		// mysql code comments say that this fuction might get called twice in a row
		// i have no idea why or when
		// so just hack around the fact for now
		if (!data_.empty())
			return 0;

		// copy whatever we need from share and release the lock
		view_t tmp_data = []()
		{
			view_t tmp_data;

			std::lock_guard<std::mutex> lk_(P_CTX_->lock);

			for (auto const& share_pair : P_CTX_->open_shares)
			{
				auto const *share = share_pair.second.get();

				// use only shares that are actually supposed to be backed by pinba report
				if (!share->report_needs_engine)
					continue;

				// and shares that have a report active
				if (!share->report_active)
					continue;

				tmp_data.emplace_back(view_row_t{
					.share_data   = *share, // a copy
					.report_state = {},
				});
			}

			return tmp_data;
		}();

		// get reports state, no lock needed here, might be slow
		// also some reports, that we've coped data for, might have been deleted meanwhile
		for (auto& row : tmp_data)
		{
			try
			{
				auto rstate = P_E_->get_report_state(row.share_data.report_name);

				assert(rstate); // the above function should throw if rstate is to be returned empty

				row.report_state = move(rstate);
				data_.emplace_back(std::move(row));
			}
			catch (std::exception const& e)
			{
				LOG_DEBUG(P_L_, "get_report_state for {0} failed (skipping), err: {1}", row.share_data.report_name, e.what());
				continue;
			}
		}

		pos_ = data_.begin();

		return 0;
	}

	virtual int rnd_end(pinba_handler_t*) override
	{
		data_.clear();
		return 0;
	}

	virtual int rnd_next(pinba_handler_t *handler, uchar *buf) override
	{
		if (pos_ == data_.end())
			return HA_ERR_END_OF_FILE;

		MEOW_DEFER(
			pos_ = std::next(pos_);
		);

		auto const *row   = &(*pos_);
		auto const *sdata = &row->share_data;
		auto       *table = handler->current_table();

		report_state_t           *rstate     = row->report_state.get();
		report_info_t const      *rinfo      = rstate->info;
		report_stats_t const     *rstats     = rstate->stats;
		report_estimates_t const *restimates = &rstate->estimates;

		// remember to lock this row stats data, since it might be changed by report host thread
		// FIXME: this probably IS too coarse!
		std::lock_guard<std::mutex> stats_lk_(rstats->lock);

		// mark all fields as writeable to avoid assert() in ::store() calls
		// got no idea how to do this properly anyway
		auto *old_map = tmp_use_all_columns(table, table->write_set);
		MEOW_DEFER(
			tmp_restore_column_map(table->write_set, old_map);
		);

		for (Field **field = table->field; *field; field++)
		{
			unsigned const field_index = (*field)->field_index;

			if (!bitmap_is_set(table->read_set, field_index))
				continue;

			switch (field_index)
			{
				case 0:
					(*field)->set_notnull();
					(*field)->store(rstate->id);
				break;

				case 1:
					(*field)->set_notnull();
					(*field)->store(sdata->mysql_name.c_str(), sdata->mysql_name.length(), &my_charset_bin);
				break;

				case 2:
					(*field)->set_notnull();
					(*field)->store(sdata->report_name.c_str(), sdata->report_name.length(), &my_charset_bin);
				break;

				case 3:
				{
					str_ref const kind_name = [&sdata]()
					{
						return (sdata->view_conf)
								? pinba_view_kind::enum_as_str_ref(sdata->view_conf->kind)
								: meow::ref_lit("!! <table comment parse error (select from it, to see the error)>");
					}();

					(*field)->set_notnull();
					(*field)->store(kind_name.data(), kind_name.c_length(), &my_charset_bin);
				}
				break;

				case 4:
				{
					auto const uptime = os_unix::clock_monotonic_now() - rstats->created_tv;
					(*field)->set_notnull();
					(*field)->store(timeval_to_double(uptime));
				}
				break;

				STORE_FIELD (5,  duration_seconds_as_double(rinfo->time_window));
				STORE_FIELD (6,  rinfo->tick_count);
				STORE_FIELD (7,  restimates->row_count);
				STORE_FIELD (8,  restimates->mem_used);
				STORE_FIELD (9,  rstats->packets_recv_total);
				STORE_FIELD (10, rstats->packets_send_err);
				STORE_FIELD (11, rstats->packets_aggregated);
				STORE_FIELD (12, rstats->packets_dropped_by_bloom);
				STORE_FIELD (13, rstats->packets_dropped_by_filters);
				STORE_FIELD (14, rstats->packets_dropped_by_rfield);
				STORE_FIELD (15, rstats->packets_dropped_by_rtag);
				STORE_FIELD (16, rstats->packets_dropped_by_timertag);
				STORE_FIELD (17, rstats->timers_scanned);
				STORE_FIELD (18, rstats->timers_aggregated);
				STORE_FIELD (19, rstats->timers_skipped_by_filters);
				STORE_FIELD (20, rstats->timers_skipped_by_tags);
				STORE_FIELD (21, timeval_to_double(rstats->ru_utime));
				STORE_FIELD (22, timeval_to_double(rstats->ru_stime));
				STORE_FIELD (23, timeval_to_double(rstats->last_tick_tv));
				STORE_FIELD (24, duration_seconds_as_double(rstats->last_tick_prepare_d));
				STORE_FIELD (25, duration_seconds_as_double(rstats->last_snapshot_merge_d));
			}
		} // field for

		return 0;
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////

struct pinba_view___report_snapshot_t : public pinba_view___base_t
{
	report_snapshot_ptr            snapshot_;
	report_snapshot_t::position_t  pos_;
	pinba_share_data_ptr           share_data_; // copied from share

	static constexpr unsigned const n_data_fields___by_request = 11;
	static constexpr unsigned const n_data_fields___by_timer   = 10;
	static constexpr unsigned const n_data_fields___by_packet  = 7;

public:

	virtual int rnd_init(pinba_handler_t *handler, bool scan) override
	{
		if (snapshot_)
			return 0;

		share_data_ = meow::make_unique<pinba_share_data_t>();

		{
			std::lock_guard<std::mutex> lk_(P_CTX_->lock);

			auto const *share = handler->current_share().get();
			*share_data_ = static_cast<pinba_share_data_t const&>(*share); // a copy
		}

		LOG_DEBUG(P_L_, "{0}; getting snapshot for t: {1}, r: {2}", __func__, share_data_->mysql_name, share_data_->report_name);

		try
		{
			snapshot_ = P_E_->get_report_snapshot(share_data_->report_name);
		}
		catch (std::exception const& e)
		{
			LOG_WARN(P_L_, "{0}; internal error: {1}", __func__, e.what());
			my_printf_error(ER_INTERNAL_ERROR, "[pinba] %s", MYF(0), e.what());
			return HA_ERR_INTERNAL_ERROR;
		}

		// check if percentile fields are being requested and do not merge histograms if not

		bool const need_percentiles = [&]()
		{
			unsigned percentile_field_min = 0;
			unsigned percentile_field_max = 0;

			auto const *view_conf = share_data_->view_conf.get();

			switch (view_conf->kind)
			{
				case pinba_view_kind::stats:
				case pinba_view_kind::active_reports:
					assert(!"must not happen");
				break;

				case pinba_view_kind::report_by_request_data:
					percentile_field_min = view_conf->keys.size() + n_data_fields___by_request;
					percentile_field_max = percentile_field_min + view_conf->percentiles.size();
				break;

				case pinba_view_kind::report_by_timer_data:
					percentile_field_min = view_conf->keys.size() + n_data_fields___by_timer;
					percentile_field_max = percentile_field_min + view_conf->percentiles.size();
				break;

				case pinba_view_kind::report_by_packet_data:
					percentile_field_min = view_conf->keys.size() + n_data_fields___by_packet;
					percentile_field_max = percentile_field_min + view_conf->percentiles.size();
				break;

				default:
					assert(!"must not be reached");
				break;
			}

			// check that a percentile field is set for reading
			auto *table = handler->current_table();
			for (Field **field = table->field; *field; field++)
			{
				unsigned const field_index = (*field)->field_index;

				if (!bitmap_is_set(table->read_set, field_index))
					continue;

				if ((field_index >= percentile_field_min) && (field_index < percentile_field_max))
					return true;
			}
			return false;
		}();

		// perform snapshot merge, this might take some time
		// TODO: write this time back to originating report's stats (non-trivial)
		{
			meow::stopwatch_t sw;

			auto const ptype = (need_percentiles)
								? report_snapshot_t::prepare_type::full
								: report_snapshot_t::prepare_type::no_histograms;
			snapshot_->prepare(ptype);

			LOG_DEBUG(P_L_, "{0}; report_snapshot for: {1}, prepare ({2}) took {3} seconds ({4} rows)",
				__func__, share_data_->mysql_name,
				report_snapshot_t::prepare_type::enum_as_str_ref(ptype),
				sw.stamp(), snapshot_->row_count());
		}

		pos_ = snapshot_->pos_first();

		return 0;
	}

	virtual int rnd_end(pinba_handler_t*) override
	{
		share_data_.reset();
		snapshot_.reset();
		return 0;
	}

	virtual int rnd_next(pinba_handler_t *handler, uchar *buf) override
	{
		if (snapshot_->pos_equal(pos_, snapshot_->pos_last()))
			return HA_ERR_END_OF_FILE;

		MEOW_DEFER(
			pos_ = snapshot_->pos_next(pos_);
		);

		auto *table       = handler->current_table();
		auto const *rinfo = snapshot_->report_info();
		auto const key    = snapshot_->get_key_str(pos_);

		unsigned const n_key_fields = rinfo->n_key_parts;

		// mark all fields as writeable to avoid assert() in ::store() calls
		// got no idea how to do this properly anyway
		auto *old_map = tmp_use_all_columns(table, table->write_set);
		MEOW_DEFER(
			tmp_restore_column_map(table->write_set, old_map);
		);

		for (Field **field = table->field; *field; field++)
		{
			unsigned const field_index = (*field)->field_index;
			unsigned       findex      = field_index;

			if (!bitmap_is_set(table->read_set, field_index))
				continue;

			// key comes first
			{
				if (findex < n_key_fields)
				{
					(*field)->set_notnull();
					(*field)->store(key[findex].begin(), key[findex].c_length(), &my_charset_bin);
					continue;
				}
				findex -= n_key_fields;
			}

			// row data comes next
			if (REPORT_KIND__BY_REQUEST_DATA == rinfo->kind)
			{
				constexpr unsigned const n_data_fields = n_data_fields___by_request;
				if (findex < n_data_fields)
				{
					auto const *row = reinterpret_cast<report_row_data___by_request_t*>(snapshot_->get_data(pos_));

					switch (findex)
					{
						STORE_FIELD(0,  row->req_count);
						STORE_FIELD(1,  double(row->req_count) / duration_seconds_as_double(rinfo->time_window));
						STORE_FIELD(2,  duration_seconds_as_double(row->time_total));
						STORE_FIELD(3,  duration_seconds_as_double(row->time_total) / duration_seconds_as_double(rinfo->time_window));
						STORE_FIELD(4,  duration_seconds_as_double(row->ru_utime));
						STORE_FIELD(5,  duration_seconds_as_double(row->ru_utime) / duration_seconds_as_double(rinfo->time_window));
						STORE_FIELD(6,  duration_seconds_as_double(row->ru_stime));
						STORE_FIELD(7,  duration_seconds_as_double(row->ru_stime) / duration_seconds_as_double(rinfo->time_window));
						STORE_FIELD(8,  row->traffic_kb);
						STORE_FIELD(9,  double(row->traffic_kb) / duration_seconds_as_double(rinfo->time_window));
						STORE_FIELD(10, row->mem_usage);
					}

					continue;
				}
				findex -= n_data_fields;
			}
			else if (REPORT_KIND__BY_TIMER_DATA == rinfo->kind)
			{
				static unsigned const n_data_fields = n_data_fields___by_timer;
				if (findex < n_data_fields)
				{
					auto const *row = reinterpret_cast<report_row_data___by_timer_t*>(snapshot_->get_data(pos_));

					switch (findex)
					{
						STORE_FIELD(0, row->req_count);
						STORE_FIELD(1, double(row->req_count) / duration_seconds_as_double(rinfo->time_window));
						STORE_FIELD(2, row->hit_count);
						STORE_FIELD(3, double(row->hit_count) / duration_seconds_as_double(rinfo->time_window));
						STORE_FIELD(4, duration_seconds_as_double(row->time_total));
						STORE_FIELD(5, duration_seconds_as_double(row->time_total) / duration_seconds_as_double(rinfo->time_window));
						STORE_FIELD(6, duration_seconds_as_double(row->ru_utime));
						STORE_FIELD(7, duration_seconds_as_double(row->ru_utime) / duration_seconds_as_double(rinfo->time_window));
						STORE_FIELD(8, duration_seconds_as_double(row->ru_stime));
						STORE_FIELD(9, duration_seconds_as_double(row->ru_stime) / duration_seconds_as_double(rinfo->time_window));
					}

					continue;
				}
				findex -= n_data_fields;
			}
			else if (REPORT_KIND__BY_PACKET_DATA == rinfo->kind)
			{
				static unsigned const n_data_fields = n_data_fields___by_packet;
				if (findex < n_data_fields)
				{
					auto const *row = reinterpret_cast<report_row_data___by_packet_t*>(snapshot_->get_data(pos_));

					switch (findex)
					{
						STORE_FIELD(0,  row->req_count);
						STORE_FIELD(1,  row->timer_count);
						STORE_FIELD(2, duration_seconds_as_double(row->time_total));
						STORE_FIELD(3, duration_seconds_as_double(row->ru_utime));
						STORE_FIELD(4, duration_seconds_as_double(row->ru_stime));
						STORE_FIELD(5,  row->traffic_kb);
						STORE_FIELD(6,  row->mem_usage);
					}

					continue;
				}
				findex -= n_data_fields;
			}
			else
			{
				LOG_ERROR(P_L_, "{0}; unknown report snapshot data_kind: {1}", __func__, rinfo->kind);
				// XXX: should we assert here or something?
			}

			// percentiles
			// TODO: calculate all required percentiles in one go
			//       performance testing shows that for short (~100 items) histograms it gives no effect whatsoever
			//       need to to test for large ones (10k+ items)
			auto const& percentiles = share_data_->view_conf->percentiles;

			unsigned const n_percentile_fields = percentiles.size();
			if (findex < n_percentile_fields)
			{
				auto const *histogram = snapshot_->get_histogram(pos_);

				// protect against percentile field in report without percentiles
				if (histogram != nullptr)
				{
					auto const percentile_d = [&]() -> duration_t
					{
						if (HISTOGRAM_KIND__HASHTABLE == rinfo->hv_kind)
						{
							auto const *hv = static_cast<histogram_t const*>(histogram);
							return get_percentile(*hv, { rinfo->hv_bucket_count, rinfo->hv_bucket_d }, percentiles[findex]);
						}
						else if (HISTOGRAM_KIND__FLAT == rinfo->hv_kind)
						{
							auto const *hv = static_cast<flat_histogram_t const*>(histogram);
							return get_percentile(*hv, { rinfo->hv_bucket_count, rinfo->hv_bucket_d }, percentiles[findex]);
						}

						assert(!"must not be reached");
						return {0};
					}();

					(*field)->set_notnull();
					(*field)->store(duration_seconds_as_double(percentile_d));
				}

				continue;
			}
			findex -= n_percentile_fields;

		} // loop over all fields

		return 0;
	}
};

pinba_view_ptr pinba_view_create(pinba_view_conf_t const& vcf)
{
	switch (vcf.kind)
	{
		case pinba_view_kind::stats:
			return meow::make_unique<pinba_view___stats_t>();

		case pinba_view_kind::active_reports:
			return meow::make_unique<pinba_view___active_reports_t>();

		case pinba_view_kind::report_by_request_data:
		case pinba_view_kind::report_by_timer_data:
		case pinba_view_kind::report_by_packet_data:
			return meow::make_unique<pinba_view___report_snapshot_t>();

		default:
			assert(!"must not be reached");
			return {};
	}
}


pinba_report_ptr pinba_view_report_create(pinba_view_conf_t const& vcf)
{
	switch (vcf.kind)
	{
		case pinba_view_kind::stats:
		case pinba_view_kind::active_reports:
			return {};

		case pinba_view_kind::report_by_packet_data:
			return create_report_by_packet(P_G_, *(vcf.get___by_packet()));

		case pinba_view_kind::report_by_request_data:
			return create_report_by_request(P_G_, *(vcf.get___by_request()));

		case pinba_view_kind::report_by_timer_data:
			return create_report_by_timer(P_G_, *(vcf.get___by_timer()));

		default:
			assert(!"must not be reached");
			return {};
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////

pinba_share_t::pinba_share_t(std::string const& table_name)
{
	thr_lock_init(&this->lock);
// XXX: this code is subtly NOT exception safe, if mysql_name assignment throws, thr lock will not be destroyed
	this->mysql_name          = table_name;
	this->report_active       = false;
	this->report_needs_engine = false;
}

pinba_share_t::~pinba_share_t()
{
	thr_lock_delete(&this->lock);
}

static pinba_share_ptr pinba_share_get_or_create_locked(char const *table_name)
{
	// P_CTX_->lock is locked here

	LOG_DEBUG(P_L_, "{0}; table_name: {1}", __func__, table_name);

	auto& open_shares = P_CTX_->open_shares;

	auto const it = open_shares.find(table_name);
	if (it != open_shares.end())
		return it->second;

	// share not found, create new one

	auto share = std::make_shared<pinba_share_t>(table_name);
	open_shares.emplace(share->mysql_name, share);

	return share;
}


static void share_init_with_table_comment_locked(pinba_share_ptr& share, str_ref table_comment)
{
	assert(!share->view_conf);
	assert(!share->report);
	assert(!share->report_active);

	share->view_conf   = pinba_view_conf_parse(share->mysql_name, table_comment);
	share->report      = pinba_view_report_create(*share->view_conf);

	if (share->report)
	{
		share->report_name         = share->mysql_name;
		share->report_active       = false;
		share->report_needs_engine = true;
	}
	else
	{
		share->report_name = ff::fmt_str("<virtual table: {0}>", pinba_view_kind::enum_as_str_ref(share->view_conf->kind));
		share->report_active       = true;
		share->report_needs_engine = false;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////

pinba_handler_t::pinba_handler_t(handlerton *hton, TABLE_SHARE *table_arg)
	: handler(hton, table_arg)
{
	LOG_DEBUG(P_L_, "{0}({1}, {2}) -> {3}", __func__, hton, table_arg, this);
}

pinba_handler_t::~pinba_handler_t()
{
	LOG_DEBUG(P_L_, "{0} <- {1}", __func__, this);
}

pinba_share_ptr pinba_handler_t::current_share() const
{
	return this->share_;
}

TABLE* pinba_handler_t::current_table() const
{
	return this->table;
}

int pinba_handler_t::create(const char *table_name, TABLE *table_arg, HA_CREATE_INFO *create_info)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	try
	{
		if (!table_arg->s || !table_arg->s->comment.str)
			throw std::runtime_error("pinba table must have a comment, please see docs");

		std::unique_lock<std::mutex> lk_(P_CTX_->lock);
		auto share = pinba_share_get_or_create_locked(table_name);

		str_ref const comment = { table_arg->s->comment.str, size_t(table_arg->s->comment.length) };
		share_init_with_table_comment_locked(share, comment);
	}
	catch (std::exception const& e)
	{
		my_printf_error(ER_CANT_CREATE_TABLE, "[pinba] %s", MYF(0), e.what());
		DBUG_RETURN(HA_WRONG_CREATE_OPTION);
	}

	DBUG_RETURN(0);
}

/**
	@brief
	Used for opening tables. The name will be the name of the file.

	@details
	A table is opened when it needs to be opened; e.g. when a request comes in
	for a SELECT on the table (tables are not open and closed for each request,
	they are cached).

	Called from handler.cc by handler::ha_open(). The server opens all tables by
	calling ha_open() which then calls the handler specific open().

	@see
	handler::ha_open() in handler.cc
*/
int pinba_handler_t::open(const char *table_name, int mode, uint test_if_locked)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	// open will always come for either
	//  - already active report (we've got 'share' open and stored in 'open_shares')
	//  - table just created and not yet active, i.e. got parsed config and created report
	//    but pinba engine is not aware of that report yet

	pinba_share_ptr share;

	{
		std::unique_lock<std::mutex> lk_(P_CTX_->lock);

		try
		{
			share = pinba_share_get_or_create_locked(table_name);

			// config NOT parsed yet (i.e. existing table after restart)
			if (!share->view_conf)
			{
				TABLE *table = current_table();

				if (!table->s || !table->s->comment.str)
					throw std::runtime_error("pinba table must have a comment, please see docs");

				str_ref const comment = { table->s->comment.str, size_t(table->s->comment.length) };
				share_init_with_table_comment_locked(share, comment);
			}
		}
		catch (std::exception const& e) // catch block should run with lock held, since we're reading from share
		{
			// this MUST not happen in fact, since all tables should have been created
			// and we do parse comments on create()
			// might happen when pinba versions are upgraded or something, i guess?

			LOG_ERROR(P_L_, "{0}; table: {1}, error: {2}", __func__, table_name, e.what());
			my_printf_error(ER_CANT_CREATE_TABLE, "[pinba] THIS IS A BUG, report! %s", MYF(0), e.what());
			DBUG_RETURN(HA_WRONG_CREATE_OPTION);
		}
	} // P_CTX_->lock released here

	// don't need to lock for this, view_conf is immutable once created
	// but allocation can throw
	this->pinba_view_ = pinba_view_create(*share->view_conf);

	// commit here, nothrow block
	thr_lock_data_init(&share->lock, &this->lock_data, (void*)this);
	this->share_ = share;

	DBUG_RETURN(0);
}

/**
	@brief
	Closes a table.

	@details
	Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
	only used to close up temporary tables or during the process where a
	temporary table is converted over to being a myisam table.

	For sql_base.cc look at close_data_tables().

	@see
	sql_base.cc, sql_select.cc and table.cc
*/
int pinba_handler_t::close(void)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	share_.reset();
	pinba_view_.reset();

	DBUG_RETURN(0);
}

/**
	@brief
	rnd_init() is called when the system wants the storage engine to do a table
	scan. See the example in the introduction at the top of this file to see when
	rnd_init() is called.

	@details
	Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
	and sql_update.cc.

	@see
	filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int pinba_handler_t::rnd_init(bool scan)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	{
		std::unique_lock<std::mutex> lk_(P_CTX_->lock);

		try
		{
			// report not active - might need to activate
			if (share_->report_needs_engine && !share_->report_active)
			{
				assert(share_->report);

				pinba_error_t const err = P_E_->add_report(share_->report);
				if (err)
					throw std::runtime_error(ff::fmt_str("can't activate report: {0}", err.what()));

				share_->report.reset(); // do not hold onto the report after activation
				share_->report_active = true;
			}
		}
		catch (std::exception const& e)
		{
			LOG_ERROR(P_L_, "{0}; table: {1}, error: {2}", __func__, share_->mysql_name, e.what());

			my_printf_error(ER_CANT_CREATE_TABLE, "[pinba] THIS IS A BUG, report! %s", MYF(0), e.what());
			DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
		}
	} // P_CTX_->lock released here

	// this should be nothrow
	int const r = pinba_view_->rnd_init(this, scan);

	DBUG_RETURN(r);
}

int pinba_handler_t::rnd_end()
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	int const r = pinba_view_->rnd_end(this);

	DBUG_RETURN(r);
}


/**
	@brief
	This is called for each row of the table scan. When you run out of records
	you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
	The Field structure for the table is the key to getting data into buf
	in a manner that will allow the server to understand it.

	@details
	Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
	and sql_update.cc.

	@see
	filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int pinba_handler_t::rnd_next(uchar *buf)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	int const r = pinba_view_->rnd_next(this, buf);

	DBUG_RETURN(r);
}

/**
	@brief
	This is like rnd_next, but you are given a position to use
	to determine the row. The position will be of the type that you stored in
	ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
	or position you saved when position() was called.

	@details
	Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and sql_update.cc.

	@see
	filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int pinba_handler_t::rnd_pos(uchar *buf, uchar *pos)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	int const r = pinba_view_->rnd_pos(this, buf, pos);

	DBUG_RETURN(r);
}

/**
	@brief
	position() is called after each call to rnd_next() if the data needs
	to be ordered. You can do something like the following to store
	the position:
	@code
	my_store_ptr(ref, ref_length, current_position);
	@endcode

	@details
	The server uses ref to store data. ref_length in the above case is
	the size needed to store current_position. ref is just a byte array
	that the server will maintain. If you are using offsets to mark rows, then
	current_position should be the offset. If it is a primary key like in
	BDB, then it needs to be a primary key.

	Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

	@see
	filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void pinba_handler_t::position(const uchar *record)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	pinba_view_->position(this, record);

	DBUG_VOID_RETURN;
}

/**
	@brief
	::info() is used to return information to the optimizer. See my_base.h for
	the complete description.

	@details
	Currently this table handler doesn't implement most of the fields really needed.
	SHOW also makes use of this data.

	You will probably want to have the following in your code:
	@code
	if (records < 2)
		records = 2;
	@endcode
	The reason is that the server will optimize for cases of only a single
	record. If, in a table scan, you don't know the number of records, it
	will probably be better to set records to two so you can return as many
	records as you need. Along with records, a few more variables you may wish
	to set are:
		records
		deleted
		data_file_length
		index_file_length
		delete_length
		check_time
	Take a look at the public variables in handler.h for more information.

	Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
	sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
	sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc,
	sql_table.cc, sql_union.cc, and sql_update.cc.

	@see
	filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc, sql_delete.cc,
	sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc, sql_select.cc,
	sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_table.cc,
	sql_union.cc and sql_update.cc
*/
int pinba_handler_t::info(uint flag)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	int const r = pinba_view_->info(this, flag);

	DBUG_RETURN(r);
}


/*
  Grab bag of flags that are sent to the able handler every so often.
  HA_EXTRA_RESET and HA_EXTRA_RESET_STATE are the most frequently called.
  You are not required to implement any of these.
*/
int pinba_handler_t::extra(enum ha_extra_function operation)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	int const r = pinba_view_->extra(this, operation);

	DBUG_RETURN(r);
}

/*
  Called by the database to lock the table. Keep in mind that this
  is an internal lock.
*/
THR_LOCK_DATA **pinba_handler_t::store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type)
{
	if (lock_type != TL_IGNORE && lock_data.type == TL_UNLOCK)
		lock_data.type = lock_type;

	*to++ = &lock_data;
	return to;
}

int pinba_handler_t::rename_table(const char *from, const char *to)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	std::lock_guard<std::mutex> lk_(P_CTX_->lock);

	auto& open_shares = P_CTX_->open_shares;

	auto it = open_shares.find(from);
	if (it == open_shares.end())
	{
		// no idea why this might happen, just try and be nice
		LOG_ERROR(P_L_, "{0}; can't find table to rename from: '{1}' (weird mysql shenanigans?)", __func__, from);
		DBUG_RETURN(0);
	}

	auto share = it->second;
	open_shares.erase(it);

	share->mysql_name = to;
	open_shares.emplace(share->mysql_name, share);

	LOG_DEBUG(P_L_, "{0}; renamed mysql table '{1}' -> '{2}', internal report_name: '{3}'",
		__func__, from, to, share->report_name);

	DBUG_RETURN(0);
}

int pinba_handler_t::delete_table(const char *table_name)
{
	DBUG_ENTER(__PRETTY_FUNCTION__);

	std::lock_guard<std::mutex> lk_(P_CTX_->lock);

	auto& open_shares = P_CTX_->open_shares;

	auto const it = open_shares.find(table_name);
	if (it == open_shares.end())
	{
		// no idea why this might happen, just try and be nice
		LOG_ERROR(P_L_, "{0}; can't find table to delete: '{1}' (weird mysql shenanigans?)", __func__, table_name);
		DBUG_RETURN(0);
	}

	auto share = it->second;
	open_shares.erase(it);

	// skip if it's just a virtual table or report that hasn't been activated yet
	if (share->report_needs_engine && share->report_active)
	{
		auto const err = P_E_->delete_report(share->report_name);
		if (err)
		{
			LOG_ERROR(P_L_, "{0}; table: '{1}', report: '{2}'; error: {3}",
				__func__, share->mysql_name, share->report_name, err.what());

			DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
		}
	}

	LOG_DEBUG(P_L_, "{0}; dropped table '{1}', report '{2}'", __func__, share->mysql_name, share->report_name);

	DBUG_RETURN(0);
}
