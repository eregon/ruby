# frozen_string_literal: true

require 'fiber'
require 'socket'

begin
  require 'io/nonblock'
rescue LoadError
  # Ignore.
end

class Scheduler
  def initialize
    @readable = {}
    @writable = {}
    @waiting = {}
    @waiting_mutex = Hash.new { |h,k| h[k] = [] }.compare_by_identity
    # @waiting_on = {}.compare_by_identity # Fiber => waiting_for
    @blocking = []

    @ios = ObjectSpace::WeakMap.new
    @thread = Thread.current
  end

  attr :readable
  attr :writable
  attr :waiting
  attr :blocking

  def next_timeout
    _fiber, timeout = @waiting.min_by{|key, value| value}

    if timeout
      offset = timeout - current_time

      if offset < 0
        return 0
      else
        return offset
      end
    end
  end

  def run
    while @readable.any? or @writable.any? or @waiting.any? or @waiting_mutex.any?
      # Can only handle file descriptors up to 1024...
      readable, writable = IO.select(@readable.keys, @writable.keys, [], next_timeout)

      # puts "readable: #{readable}" if readable&.any?
      # puts "writable: #{writable}" if writable&.any?

      readable&.each do |io|
        @readable[io]&.resume
      end

      writable&.each do |io|
        @writable[io]&.resume
      end

      if @waiting.any?
        time = current_time
        waiting = @waiting
        @waiting = {}

        waiting.each do |fiber, timeout|
          if timeout <= time
            fiber.resume
          else
            @waiting[fiber] = timeout
          end
        end
      end
    end
  end

  def for_fd(fd)
    @ios[fd] ||= ::IO.for_fd(fd, autoclose: false)
  end

  def wait_readable(io)
    @readable[io] = Fiber.current

    Fiber.yield

    @readable.delete(io)

    return true
  end

  def wait_readable_fd(fd)
    wait_readable(for_fd(fd))
  end

  def wait_writable(io)
    @writable[io] = Fiber.current

    Fiber.yield

    @writable.delete(io)

    return true
  end

  def wait_writable_fd(fd)
    wait_writable(for_fd(fd))
  end

  def current_time
    Process.clock_gettime(Process::CLOCK_MONOTONIC)
  end

  def wait_sleep(duration = nil)
    @waiting[Fiber.current] = current_time + duration

    Fiber.yield

    return true
  end

  def wait_any(io, events, duration)
    unless (events & IO::WAIT_READABLE).zero?
      @readable[io] = Fiber.current
    end

    unless (events & IO::WAIT_WRITABLE).zero?
      @writable[io] = Fiber.current
    end

    Fiber.yield

    @readable.delete(io)
    @writable.delete(io)

    return true
  end

  def wait_for_single_fd(fd, events, duration)
    wait_any(for_fd(fd), events, duration)
  end

  # called from Mutex#lock
  def wait_mutex(mutex)
    p [:wait_mutex, mutex]
    # @waiting_on[Fiber.current] = mutex

    # Guarantee: Fiber.current is a Fiber of current Thread and is a Fiber of this scheduler
    @waiting_mutex[mutex] << Fiber.current
    Fiber.yield
    true
  end

  # called from Mutex#unlock, possibly from another Fiber on the same thread, or possibly from some other thread
  def notify_mutex(mutex)
    p [:notify_mutex, mutex]
    # p @waiting_mutex
    q = @waiting_mutex[mutex]
    fiber = q.shift
    @waiting_mutex.delete(mutex) if q.empty?
    raise unless fiber

    if Thread.current == @thread
      # but what if fiber died in the meanwhile?
      # Is it possible if same thread? Yes, due to Fiber#raise
      # And what if it somehow went further in the meanwhile, due to e.g. Fiber#raise, we'll resume another point?
      if fiber.alive?
        p [:resume, fiber]
        fiber.resume
      end
    else
      # scheduler = Thread.current.scheduler
      # send -> { notify_mutex(mutex) } to scheduler Queue + control pipe write to wake up?
      # (could be abstracted as #external_notification(&block) ?)
      # or maybe shouldn't come at all in #notify_mutex ? => seems simpler
      # Best seems to remember scheduler+Mutex instance (+Fiber?) in `waitq` for non-blocking waiters,
      # so we know exactly how to wake up a given waiter Fiber.
      raise "different thread"
    end
  end

  # BTW, super unsafe if e.g. some IO like DB connection is shared between Fibers, no?
  # (e.g., send, send, recv, recv, might be reordered)

  def enter_blocking_region
    # puts "Enter blocking region: #{caller.first}"
  end

  def exit_blocking_region
    # puts "Exit blocking region: #{caller.first}"
    @blocking << caller.first
  end

  def fiber(&block)
    fiber = Fiber.new(blocking: false, &block)

    fiber.resume

    return fiber
  end
end
