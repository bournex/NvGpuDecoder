#pragma once
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
using namespace std;

/**
* Description: thread-safe circle queue
*/

template<class T, unsigned int batch_size = 8, unsigned int batch_cnt = 4 >
class circle_batch
{
public:
	typedef void(*batchcb)(T *ts, unsigned int nlen, void *user);

	circle_batch(batchcb bcb, void *cbv, unsigned int bs = batch_size, unsigned int bc = batch_cnt)
		: _widx(0), _bidx(0), _bcb(bcb), _cbv(cbv), _batch_size(bs), _batch_cnt(bc)
	{
		_q.resize(_batch_size * _batch_cnt);
	}

	~circle_batch() {}

	bool push(T &t)
	{
		/**
		* Description: push to circle batch queue
		*/
		_mtx.lock();

		/* comment shows when _batch_size = 12/_batch_cnt = 4 the pipe looks like */

		/*	  _bidx 0	  _bidx 1	   _bidx 2	   _bidx 3		*/
		/*		  ¡ý												*/
		/* +------------+-----------+------------+------------+ */
		/* |############|###		|			 |			  | */
		/* +------------+-----------+------------+------------+ */
		/*		  ¡ý			¡ü	¡ý								*/
		/*		  ¡ý		_widx	¡ý								*/
		/*	overflow push	timed push							*/

		_q[_widx++] = t;
		_widx = (_widx == _batch_size * _batch_cnt) ? 0 : _widx;

#if (__cplusplus >= 201103L)
		static thread_local vector<T> _swap(_batch_size);
#else
		if (_swap.capacity() == 0)
			_swap.resize(_batch_size);
#endif

		if ((_widx < (_bidx * _batch_size)) || (_widx >= (_bidx * _batch_size + _batch_size)))
		{
			/* out of current batch boundary */
			std::lock_guard<std::mutex> lk(_mtxswap);
			std::swap_ranges(
				_q.begin() + (_bidx * _batch_size),
				_q.begin() + (_bidx * _batch_size + _batch_size),
				_swap.begin());

			_bidx = (++_bidx == _batch_cnt) ? 0 : _bidx;

			return true;
		}
		_mtx.unlock();
		return false;
	}

	inline void push_swap()
	{
		if (_bcb)
		{
			_bcb(&_swap[0], _batch_size, _cbv);
			_swap.clear();
			_swap.resize(_batch_size);
		}
		_mtx.unlock();
	}

	/**
	* Description: force push
	*/
	unsigned int push()
	{
		_mtx.lock();

#if (__cplusplus >= 201103L)
		static thread_local vector<T> _swap(_batch_size);
#else
		// static vector<T> _swap(_batch_size);
		if (_swap.capacity() == 0)
			_swap.resize(_batch_size);
#endif

		if ((_widx < (_bidx * _batch_size)) || (_widx >= (_bidx * _batch_size + _batch_size)))
		{
			/**
			* Description: _widx out of current batch, push current batch
			*/
			std::lock_guard<std::mutex> lk(_mtxswap);
			std::swap_ranges(
				_q.begin() + (_bidx * _batch_size),
				_q.begin() + (_bidx * _batch_size + _batch_size),
				_swap.begin());

			_bidx = (++_bidx == _batch_cnt) ? 0 : _bidx;

			_mtx.unlock();

			if (_bcb)
			{
				_bcb(&_swap[0], _batch_size, _cbv);
				_swap.clear();
				_swap.resize(_batch_size);
			}
			return _batch_size;
		}
		else
		{
			/**
			* Description: _widx in current batch, push part of batch
			*/
			unsigned int pos = (_bidx == 0) ? _widx : (_widx % _batch_size);

			if (pos != 0)
			{
				std::lock_guard<std::mutex> lk(_mtxswap);
				std::swap_ranges(
					_q.begin() + (_bidx * _batch_size),
					_q.begin() + (_bidx * _batch_size + pos),
					_swap.begin());

				_bidx = (++_bidx == _batch_cnt) ? 0 : _bidx;
				_widx = _bidx * _batch_size;

				_mtx.unlock();

				if (_bcb)
				{
					_bcb(&_swap[0], pos, _cbv);
					_swap.clear();
					_swap.resize(_batch_size);
				}
				return pos;
			}
			else
			{
				/**
				* Description: _widx is at the beginning of current batch, nothing to push
				*/
			}
		}
		_mtx.unlock();
		return 0;
	}

private:
	vector<T>		_q;				/* batch queue */
	vector<T>		_swap;			/* swap batch */
	std::mutex		_mtx;			/* lock for queue batch pipe */
	std::mutex		_mtxswap;		/* lock for swap batch */
	batchcb			_bcb;			/* user callback */
	void*			_cbv;			/* user callback data */
	unsigned int	_widx;			/* batch writing index */
	unsigned int	_bidx;			/* batch index */
	unsigned int	_batch_size;	/* batch size */
	unsigned int	_batch_cnt;		/* batch count */
};