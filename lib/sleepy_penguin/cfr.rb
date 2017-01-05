module SleepyPenguin
  def self.copy_file_range(io_in, io_out, len, flags = 0,
                           off_in: nil, off_out: nil)
    __cfr(io_in, off_in, io_out, off_out, len, flags)
  end
end
