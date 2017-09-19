/*!
  @file MicroTimer.h
  @author Susmit Biswas
  @version 1.0
  @date 2009-2010

  @brief Contains definitions of fine grain timer
 */

#ifndef __MICROTIMER_H__
#define __MICROTIMER_H__

#include <iostream>
#include <iomanip>
#include <cstring>
#include <sys/time.h>

/*! @brief Collects fine grain timing stats using gettimeofday() */
class MicroTimer {
	public:
		/*! @brief starts the timer */
		virtual void Start ();

		/*! @brief stops the timer */
		virtual void Stop ();

		/*! @brief computes time
		  * @return difference of time between stop() and start() calls
		 */
		virtual unsigned long GetDiff() const;

		/*! @brief prints the timer */
		friend std::ostream& operator << (std::ostream& os, const MicroTimer& mt);

	private:
		timeval start_; /*< start time */
		timeval end_; /*< end time */
		timeval diff_; /*< (end time - start time) */
		
		/*! @brief stores (end time - start time) */
		void ComputeDiff ();
};

#endif /* __MICROTIMER_H__ */
