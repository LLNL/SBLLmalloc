/*!
  @file MicroTimer.cpp
  @author Susmit Biswas
  @version 1.0
  @date 2009-2010

  @brief Contains implementation of fine grain timer. You may not need to
  change this code. It uses gettimeofday() to obtain the duration of some
  operation. At the beginning of the code block, please declare a timer, start
  it and at the end of the block, stop it and get the duration by calling
  GetDiff.

  e.g.

  \code
  MicroTimer mt;
  mt.Start();
  {
  // Code Block
  ...
  }
  mt.Stop();

  double duration_in_usec = mt.GetDiff();
  \endcode
 */

#include "MicroTimer.h"

inline void MicroTimer::Start () { 
	gettimeofday (&start_, NULL); 
}

inline void MicroTimer::stop () {
	gettimeofday (&end_, NULL);
	ComputeDiff();
	memset (&start_, 0, sizeof (timeval));
	memset (&end_, 0, sizeof (timeval));
}

inline unsigned long MicroTimer::GetDiff () const {
	return diff_.tv_sec * 1e6 + diff_.tv_usec;
}

void MicroTimer::ComputeDiff() {
	diff_.tv_usec = end_.tv_usec - start_.tv_usec;
	diff_.tv_sec  = end_.tv_sec  - start_.tv_sec;

	if (diff_.tv_usec < 0) {
		diff_.tv_sec--;
		diff_.tv_usec = 1e6 - m_start.tv_usec + m_end.tv_usec;
	}
}

std::ostream & operator << (std::ostream& os, const MicroTimer& mt)
{
	os << std::setw (10) << mt.GetDiff () << " us";
	return os;
}
