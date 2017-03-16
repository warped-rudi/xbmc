/*
 *      Copyright (C) 2012 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#pragma once

template <class T, int S>
class FastFiFo
{
  int  head;
  int  tail;
  T    data[S];

public:
  FastFiFo() { tail = head = 0; }

  const int getCapacity() const { return S-1; }
  bool isEmpty() const { return head == tail; }
  bool isFull() const { return ((tail + 1) % S) == head; }

  int usedCount() const
  {
    int c = tail - head;
    return ( c >= 0 ) ?  c : c + S;
  }

  int freeCount() const
  {
    int c = head - tail - 1;
    return ( c >= 0 ) ?  c : c + S;
  }

  void flushAll() { head = tail; }


  const T *peekHeadPtr() const
  {
    return isEmpty() ?  0 : &data[head];
  }

  T peekHead() const
  {
    return isEmpty() ?  T(0) : data[head];
  }

  bool nextHead()
  {
    if( !isEmpty() )
    {
      head = (head + 1) % S;
      return true;
    }

    return false;
  }

  bool getHead(T &item)
  {
    if( !isEmpty() )
    {
      item = data[head];
      head = (head + 1) % S;
      return true;
    }

    item = T(0);
    return false;
  }

  bool putTail(const T &item)
  {
    int next = (tail + 1) % S;

    if(next != head)
    {
      data[tail] = item;
      tail = next;
      return true;
    }

    return false;
  }

  bool putTail(const T *item)
  {
    int next = (tail + 1) % S;

    if(next != head)
    {
      data[tail] = *item;
      tail = next;
      return true;
    }

    return false;
  }
};
