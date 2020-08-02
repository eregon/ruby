require_relative 'test/fiber/scheduler'

mutex = Mutex.new
scheduler = Scheduler.new
Thread.current.scheduler = scheduler
r, w = IO.pipe

p Thread.current

t = Thread.new do
  # Fiber do
    mutex.synchronize do
      puts "in synchronize, before yield"
      r.read(1)
      # sleep 1
      puts "in synchronize, after yield"
    end
    puts "unlocked"
  # end
end

Thread.pass until mutex.locked?

Fiber do
  puts "before lock"
  mutex.lock
  puts "after lock"
end

Fiber do
  w.write('a')
end

scheduler.run

t.join
