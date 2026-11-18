import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Observable;
import java.util.Observer;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

import org.apache.log4j.AppenderSkeleton;
import org.apache.log4j.ConsoleAppender;
import org.apache.log4j.Level;
import org.apache.log4j.Logger;
import org.apache.log4j.SimpleLayout;
import org.apache.log4j.spi.LoggingEvent;
import org.contikios.cooja.Cooja;
import org.contikios.cooja.Mote;
import org.contikios.cooja.Simulation;
import org.contikios.cooja.TimeEvent;
import org.contikios.cooja.contikimote.ContikiMote;
import org.contikios.cooja.interfaces.Log;
import org.contikios.cooja.interfaces.Radio;
import org.contikios.cooja.interfaces.Radio.RadioEvent;
import org.contikios.cooja.mote.memory.MemoryInterface.Symbol;
import org.contikios.cooja.mote.memory.VarMemory;

/** Headless entry point for the repository's legacy Cooja scenarios. */
public final class GTSFinalRunner {
  private static final String[] REQUIRED = {
      "simLoggedLength", "simLoggedFlag", "simLoggedData",
      "simOutSize", "simOutDataBuffer", "simInSize", "simInDataBuffer",
      "simRadioHWOn", "simSerialReceivingLength", "simSerialReceivingData"
  };
  private static final long WALL_WATCHDOG_MS = 600_000;
  private static final long SIM_SAFETY_MARGIN_US = 1_000_000;

  public static void main(String[] args) throws Exception {
    Logger root = Logger.getRootLogger();
    root.removeAllAppenders();
    root.addAppender(new ConsoleAppender(new SimpleLayout()));
    root.setLevel(Level.INFO);
    if (args.length != 3) {
      throw new IllegalArgumentException("Usage: GTSFinalRunner <scenario> <external-tools-config> <duration-us>");
    }

    long durationUs = Long.parseLong(args[2]);
    Cooja.externalToolsUserSettingsFile = new File(args[1]);
    Simulation simulation = Cooja.quickStartSimulationConfig(new File(args[0]), false, null);
    if (simulation == null) throw new IllegalStateException("Cooja failed to load the simulation configuration");

    AtomicLong scenarioTimeoutAt = new AtomicLong(-1);
    AtomicReference<String> runtimeError = new AtomicReference<>();
    AppenderSkeleton logCapture = new AppenderSkeleton() {
      @Override protected void append(LoggingEvent event) {
        String message = String.valueOf(event.getRenderedMessage());
        if (message.startsWith("Timeout event @ ")) {
          try { scenarioTimeoutAt.set(Long.parseLong(message.substring("Timeout event @ ".length()).trim())); }
          catch (NumberFormatException ignored) { runtimeError.compareAndSet(null, "unparseable scenario timeout event"); }
        }
        if (event.getLevel().isGreaterOrEqual(Level.ERROR)) runtimeError.compareAndSet(null, message);
      }
      @Override public void close() { }
      @Override public boolean requiresLayout() { return false; }
    };
    root.addAppender(logCapture);

    AtomicInteger logCallbacks = new AtomicInteger();
    AtomicInteger radioTx = new AtomicInteger();
    AtomicInteger radioRx = new AtomicInteger();
    List<Observer> observers = new ArrayList<>();
    boolean started = false;
    boolean wallWatchdog = false;
    boolean runnerSafetyStop = false;
    long finalTime = simulation.getSimulationTime();
    String failure = null;

    try {
      Mote[] motes = simulation.getMotes();
      if (motes.length != 3) throw new IllegalStateException("Expected exactly three motes, loaded " + motes.length);
      for (Mote mote : motes) {
        if (!(mote instanceof ContikiMote)) throw new IllegalStateException("Mote " + mote.getID() + " is not a Contiki mote");
        VarMemory memory = new VarMemory(((ContikiMote) mote).getMemory());
        for (String name : REQUIRED) {
          if (!memory.variableExists(name)) throw new IllegalStateException("Missing required memory symbol " + name + " on mote " + mote.getID());
          Symbol symbol = memory.getVariable(name);
          if (symbol.type != Symbol.Type.VARIABLE) throw new IllegalStateException("Required symbol is not a variable: " + name);
          if (symbol.addr < 0) throw new IllegalStateException("Invalid negative address for " + name + " on mote " + mote.getID());
          if (symbol.size == 0) throw new IllegalStateException("Invalid zero-sized symbol metadata for " + name + " on mote " + mote.getID());
          boolean addressValid = memory.getByteArray(symbol.addr, 1).length == 1;
          if (!addressValid) throw new IllegalStateException("Symbol address is outside mote memory: " + name + " on mote " + mote.getID());
          String sizeStatus = symbol.size > 0 ? "known" : "unknown";
          System.out.println("MEMORY_SYMBOL mote=" + mote.getID() + " name=" + name
              + " symbolExists=true addressValid=true address=0x" + Long.toHexString(symbol.addr)
              + " sizeKnown=" + (symbol.size > 0) + " sizeStatus=" + sizeStatus + " size=" + symbol.size);
        }

        Log log = mote.getInterfaces().getLog();
        if (log == null) throw new IllegalStateException("Mote " + mote.getID() + " has no log interface");
        Observer logObserver = new Observer() {
          @Override public void update(Observable source, Object value) {
            String message = log.getLastLogMessage();
            if (message == null) return;
            logCallbacks.incrementAndGet();
            System.out.println("MOTE_LOG time_us=" + simulation.getSimulationTime() + " id=" + mote.getID()
                + " " + message.replace('\n', ' ').replace('\r', ' '));
          }
        };
        log.addObserver(logObserver);
        observers.add(logObserver);

        Radio radio = mote.getInterfaces().getInterfaceOfType(Radio.class);
        if (radio == null) throw new IllegalStateException("Mote " + mote.getID() + " has no radio interface");
        Observer radioObserver = new Observer() {
          @Override public void update(Observable source, Object value) {
            RadioEvent event = radio.getLastEvent();
            if (event == RadioEvent.PACKET_TRANSMITTED) radioTx.incrementAndGet();
            if (event == RadioEvent.RECEPTION_FINISHED) radioRx.incrementAndGet();
          }
        };
        radio.addObserver(radioObserver);
        observers.add(radioObserver);
        System.out.println("MOTE_INITIALIZED id=" + mote.getID() + " type=ContikiMote requiredSymbols=" + REQUIRED.length);
      }

      System.out.println("CONFIG_LOADED motes=" + motes.length + " seed=" + simulation.getRandomSeed());
      if (simulation.getRandomSeed() != 123456) throw new IllegalStateException("Unexpected random seed " + simulation.getRandomSeed());

      /* The scenario's Rhino TIMEOUT is primary. This later event is a safety stop if it fails. */
      TimeEvent safetyStop = new TimeEvent(0, "Codespaces simulation safety watchdog") {
        @Override public void execute(long time) {
          System.out.println("RUNNER_SAFETY_STOP time_us=" + time);
          simulation.stopSimulation(false);
        }
      };
      long safetyAt = durationUs + SIM_SAFETY_MARGIN_US;
      System.out.println("SIM_DURATION_START time_us=" + simulation.getSimulationTime() + " duration_us=" + durationUs
          + " scenario_timeout_expected_us=" + durationUs + " runner_safety_at_us=" + safetyAt);
      simulation.scheduleEvent(safetyStop, safetyAt);
      simulation.startSimulation();
      started = true;

      long wallDeadline = System.currentTimeMillis() + WALL_WATCHDOG_MS;
      while (simulation.isRunning() && System.currentTimeMillis() < wallDeadline) Thread.sleep(100);
      if (simulation.isRunning()) {
        wallWatchdog = true;
        System.out.println("WALL_WATCHDOG_STOP elapsed_limit_ms=" + WALL_WATCHDOG_MS);
        simulation.stopSimulation(false);
        long stopDeadline = System.currentTimeMillis() + 5_000;
        while (simulation.isRunning() && System.currentTimeMillis() < stopDeadline) Thread.sleep(25);
      }
      if (simulation.isRunning()) throw new IllegalStateException("Cooja simulation thread did not stop after watchdog request");

      finalTime = simulation.getSimulationTime();
      long timeoutAt = scenarioTimeoutAt.get();
      runnerSafetyStop = finalTime >= safetyAt;
      String termination = timeoutAt >= 0 ? "scenario_timeout" : runnerSafetyStop ? "runner_safety_stop" : wallWatchdog ? "wall_watchdog" : "early_stop";
      System.out.println("TERMINATION source=" + termination + " scenario_timeout_at_us=" + timeoutAt
          + " runner_safety_stop=" + runnerSafetyStop + " wall_watchdog=" + wallWatchdog);

      if (finalTime < durationUs) failure = "simulation ended before target duration";
      else if (timeoutAt != durationUs) failure = "scenario timeout event missing or at unexpected time";
      else if (runnerSafetyStop || wallWatchdog) failure = "safety watchdog, not scenario timeout, ended the simulation";
      else if (runtimeError.get() != null) failure = "Cooja runtime error: " + runtimeError.get();
      else if (logCallbacks.get() == 0) failure = "no mote log callbacks observed";
      else if (radioTx.get() == 0 || radioRx.get() == 0) failure = "radio TX or RX callbacks missing";
    } catch (Throwable error) {
      failure = error.getClass().getSimpleName() + ": " + String.valueOf(error.getMessage());
      error.printStackTrace(System.err);
    } finally {
      if (simulation.isRunning()) {
        simulation.stopSimulation(false);
        long stopDeadline = System.currentTimeMillis() + 5_000;
        while (simulation.isRunning() && System.currentTimeMillis() < stopDeadline) {
          try { Thread.sleep(25); } catch (InterruptedException e) { Thread.currentThread().interrupt(); break; }
        }
      }
      for (Observer observer : observers) {
        for (Mote mote : simulation.getMotes()) {
          Log log = mote.getInterfaces().getLog();
          if (log != null) log.deleteObserver(observer);
          Radio radio = mote.getInterfaces().getInterfaceOfType(Radio.class);
          if (radio != null) radio.deleteObserver(observer);
        }
      }
      root.removeAppender(logCapture);
      logCapture.close();
      finalTime = simulation.getSimulationTime();
    }

    boolean passed = failure == null && started && finalTime >= durationUs;
    System.out.println("CALLBACK_COUNTS logCallbacks=" + logCallbacks.get() + " radio_tx=" + radioTx.get()
        + " radio_rx=" + radioRx.get());
    System.out.println("RUN_RESULT motes=" + simulation.getMotesCount() + " simTime=" + finalTime
        + " targetTime=" + durationUs + " durationReached=" + (finalTime >= durationUs)
        + " pass=" + passed + " failure=" + (failure == null ? "none" : failure.replace(' ', '_')));
    if (!passed) throw new IllegalStateException("Cooja run failed validation: " + failure);
  }
}
