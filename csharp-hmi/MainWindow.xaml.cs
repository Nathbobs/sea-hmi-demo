using System;
using System.IO;
using System.Windows;
using System.Windows.Threading;
using EasyModbus;

namespace SeaHmiDemo
{
    public partial class MainWindow : Window
    {
        private const string PlcHost = "127.0.0.1";
        private const int PlcPort = 502;

        // Modbus addresses -- must match cpp-simulator/src/main.cpp exactly.
        private const int RegFillLevel = 0;
        private const int RegTemperature = 1;
        private const int RegPhase = 2;
        private const int CoilAlarm = 0;
        private const int CoilControl = 1;

        private static readonly string[] PhaseNames =
        {
            "Idle (대기)",
            "Filling (충전 중)",
            "Heating (가열 중)",
            "Draining (배수 중)",
        };

        private readonly ModbusClient _modbusClient;
        private readonly DispatcherTimer _pollTimer;
        private readonly string _logFilePath;

        private int _lastLoggedPhase = -1;
        private bool _lastLoggedAlarm = false;

        public MainWindow()
        {
            InitializeComponent();

            _modbusClient = new ModbusClient(PlcHost, PlcPort);
            _logFilePath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "equipment_log.txt");

            _pollTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(100),
            };
            _pollTimer.Tick += PollTimer_Tick;
            _pollTimer.Start();
        }

        private void PollTimer_Tick(object sender, EventArgs e)
        {
            try
            {
                if (!_modbusClient.Connected)
                {
                    _modbusClient.Connect();
                }

                int[] registers = _modbusClient.ReadHoldingRegisters(RegFillLevel, 3);
                bool[] coils = _modbusClient.ReadCoils(CoilAlarm, 2);

                int fillLevel = registers[RegFillLevel];
                int temperature = registers[RegTemperature];
                int phase = registers[RegPhase];
                bool alarm = coils[0];

                ConnectionStatusText.Text = "Connected (연결됨)";
                UpdateReadout(fillLevel, temperature, phase, alarm);
                CheckForLogEvents(phase, alarm);
            }
            catch (Exception)
            {
                ConnectionStatusText.Text = "Disconnected (연결 안됨)";
            }
        }

        private void UpdateReadout(int fillLevel, int temperature, int phase, bool alarm)
        {
            int clampedPhase = Math.Max(0, Math.Min(phase, PhaseNames.Length - 1));
            PhaseValueText.Text = PhaseNames[clampedPhase];
            TemperatureValueText.Text = $"{temperature} °C";
            FillLevelBar.Value = fillLevel;
            FillLevelValueText.Text = $"{fillLevel}%";
            AlarmBanner.Visibility = alarm ? Visibility.Visible : Visibility.Collapsed;
        }

        private void CheckForLogEvents(int phase, bool alarm)
        {
            if (phase != _lastLoggedPhase)
            {
                int clampedPhase = Math.Max(0, Math.Min(phase, PhaseNames.Length - 1));
                WriteLogEvent($"Phase changed to {PhaseNames[clampedPhase]}");
                _lastLoggedPhase = phase;
            }

            if (alarm != _lastLoggedAlarm)
            {
                WriteLogEvent(alarm
                    ? "ALARM: Temperature exceeded 65C (경보: 온도 초과)"
                    : "Alarm cleared (경보 해제)");
                _lastLoggedAlarm = alarm;
            }
        }

        private void WriteLogEvent(string message)
        {
            string line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} - {message}";
            EventLogListBox.Items.Insert(0, line);
            File.AppendAllText(_logFilePath, line + Environment.NewLine);
        }

        private void StartButton_Click(object sender, RoutedEventArgs e)
        {
            WriteControlCoil(true);
        }

        private void StopButton_Click(object sender, RoutedEventArgs e)
        {
            WriteControlCoil(false);
        }

        private void ResetButton_Click(object sender, RoutedEventArgs e)
        {
            WriteControlCoil(false);
        }

        private void WriteControlCoil(bool value)
        {
            try
            {
                if (!_modbusClient.Connected)
                {
                    _modbusClient.Connect();
                }

                _modbusClient.WriteSingleCoil(CoilControl, value);
            }
            catch (Exception)
            {
                ConnectionStatusText.Text = "Disconnected (연결 안됨)";
            }
        }
    }
}
