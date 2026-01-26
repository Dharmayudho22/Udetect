const express = require('express');
const mongoose = require('mongoose');
const bodyParser = require('body-parser');
const cors = require('cors');
const { spawn } = require('child_process');
const path = require('path');

const app = express();
app.use(cors());
app.use(bodyParser.json());

// Koneksi MongoDB
mongoose.connect('mongodb://localhost:27017/iot_urine', {
    useNewUrlParser: true,
    useUnifiedTopology: true
})
.then(() => console.log(" MongoDB Connected"))
.catch(err => console.log(" MongoDB Error:", err));

// Schema MongoDB dengan Machine Learning Prediction
const SensorSchema = new mongoose.Schema({
    // Data Sensor
    ph: Number,
    tds: Number,
    specificGravity: Number,
    turbidityLevel: {
        type: String,
        enum: ['Jernih', 'Agak Keruh', 'Keruh', 'N/A'],
        default: 'N/A'
    },
    turbidityNTU: Number,
    turbidityInterpretation: String,
    red: Number,
    green: Number,
    blue: Number,
    warnaDasar: String,
    analisis: String,
    mode: String,
    sequence: String,
    
    // Machine Learning Prediction
    mlPrediction: {
        disease: { type: String, default: null },
        confidence: { type: Number, default: null },
        rfPrediction: { type: String, default: null },
        rfConfidence: { type: Number, default: null },
        xgbPrediction: { type: String, default: null },
        xgbConfidence: { type: Number, default: null },
        recommendation: { type: String, default: null },
        riskLevel: { 
            type: String, 
            enum: ['Normal', 'Rendah', 'Sedang', 'Tinggi', null],
            default: null 
        },
        processedAt: { type: Date, default: null }
    },
    
    timestamp: { type: Date, default: Date.now }
});

const SensorData = mongoose.model('SensorData', SensorSchema, 'sensordataaaa');

// Fungsi validasi tingkat kekeruhan
function normalizeTurbidityLevel(level) {
    if (!level || level === 'N/A') return 'N/A';
    
    const levelUpper = level.toUpperCase();
    
    if (levelUpper.includes('JERNIH') || levelUpper.includes('BENING')) {
        return 'Jernih';
    } else if (levelUpper.includes('AGAK') || levelUpper.includes('SEDIKIT')) {
        return 'Agak Keruh';
    } else if (levelUpper.includes('KERUH')) {
        return 'Keruh';
    }
    
    return 'N/A';
}

// Fungsi untuk menjalankan ML prediction
async function runMLPrediction(dataId) {
    return new Promise((resolve, reject) => {
        console.log(` Running ML prediction for ID: ${dataId}`);
        
        const fs = require('fs');
        const pythonScript = path.join(__dirname, 'ml_predict.py');
        
        // Check if script exists
        if (!fs.existsSync(pythonScript)) {
            console.error(` ML script not found: ${pythonScript}`);
            reject({ success: false, error: 'ML script not found' });
            return;
        }
        
        // Try python3 first, fallback to python
        const pythonCmd = process.platform === 'win32' ? 'python' : 'python3';
        
        console.log(`    Script: ${pythonScript}`);
        console.log(`    Command: ${pythonCmd} ml_predict.py ${dataId}`);
        
        const pythonProcess = spawn(pythonCmd, [pythonScript, dataId], {
            cwd: __dirname
        });
        
        let outputData = '';
        let errorData = '';
        
        pythonProcess.stdout.on('data', (data) => {
            const output = data.toString();
            outputData += output;
            // Print setiap line output
            output.split('\n').forEach(line => {
                if (line.trim()) console.log(`   [ML] ${line}`);
            });
        });
        
        pythonProcess.stderr.on('data', (data) => {
            const error = data.toString();
            errorData += error;
            error.split('\n').forEach(line => {
                if (line.trim()) console.error(`   [ML ERROR] ${line}`);
            });
        });
        
        pythonProcess.on('close', (code) => {
            if (code === 0) {
                console.log(` ML prediction completed for ID: ${dataId}`);
                resolve({ success: true, output: outputData });
            } else {
                console.error(` ML prediction failed with exit code ${code}`);
                if (errorData) console.error(`   Error: ${errorData}`);
                reject({ success: false, error: errorData, code: code });
            }
        });
        
        pythonProcess.on('error', (err) => {
            console.error(` Failed to start Python process: ${err.message}`);
            console.error(`   Tried command: ${pythonCmd}`);
            reject({ success: false, error: err.message });
        });
        
        // Timeout 60 detik
        setTimeout(() => {
            if (!pythonProcess.killed) {
                pythonProcess.kill();
                console.error(`ML prediction timeout for ID: ${dataId}`);
                reject({ success: false, error: 'Timeout' });
            }
        }, 60000);
    });
}

// Rute POST data dari ESP8266 dengan auto ML prediction
app.post('/api/data', async (req, res) => {
    try {
        let { 
            ph, tds, specificGravity, 
            turbidityLevel, turbidityNTU, turbidityInterpretation,
            red, green, blue, warnaDasar, analisis, mode, sequence 
        } = req.body;

        // Pastikan nilai numerik
        red = Number(red) || 0;
        green = Number(green) || 0;
        blue = Number(blue) || 0;
        ph = Number(ph) || 0;
        tds = Number(tds) || 0;
        specificGravity = Number(specificGravity) || 0;
        turbidityNTU = Number(turbidityNTU) || 0;

        // Normalisasi tingkat kekeruhan
        const normalizedLevel = normalizeTurbidityLevel(turbidityLevel);

        // Simpan ke MongoDB
        const data = new SensorData({
            ph, tds, specificGravity,
            turbidityLevel: normalizedLevel,
            turbidityNTU,
            turbidityInterpretation: turbidityInterpretation || 'N/A',
            red, green, blue,
            warnaDasar: warnaDasar || 'N/A',
            analisis: analisis || 'N/A',
            mode: mode || 'sequential',
            sequence: sequence || 'color_then_probe'
        });

        await data.save();
        
        console.log("Data tersimpan:", {
            timestamp: new Date().toLocaleString('id-ID'),
            id: data._id,
            ph: ph.toFixed(2),
            tds: tds.toFixed(1),
            sg: specificGravity.toFixed(3),
            turbidity: `${normalizedLevel} (${turbidityNTU.toFixed(1)} NTU)`
        });
        
        // Jalankan ML prediction secara asynchronous
        runMLPrediction(data._id.toString())
            .then(() => console.log(`ML prediction queued for ID: ${data._id}`))
            .catch(err => console.error(`ML prediction error: ${err.error}`));
        
        res.status(200).json({ 
            message: "Data berhasil disimpan",
            id: data._id,
            mlProcessing: true
        });
    } catch (err) {
        console.error("Error saat simpan data:", err);
        res.status(500).json({ error: "Gagal menyimpan data" });
    }
});

// Rute POST untuk update prediksi ML
app.post('/api/prediction/:id', async (req, res) => {
    try {
        const { id } = req.params;
        const { 
            disease, confidence, 
            rfPrediction, rfConfidence, 
            xgbPrediction, xgbConfidence,
            recommendation, riskLevel
        } = req.body;

        const data = await SensorData.findByIdAndUpdate(
            id,
            {
                mlPrediction: {
                    disease: disease,
                    confidence: Number(confidence),
                    rfPrediction: rfPrediction,
                    rfConfidence: Number(rfConfidence),
                    xgbPrediction: xgbPrediction,
                    xgbConfidence: Number(xgbConfidence),
                    recommendation: recommendation,
                    riskLevel: riskLevel,
                    processedAt: new Date()
                }
            },
            { new: true }
        );

        if (!data) {
            return res.status(404).json({ 
                success: false, 
                message: "Data tidak ditemukan" 
            });
        }

        console.log(` ML Prediction updated for ID: ${id}`);
        console.log(`   ├─ Disease: ${disease}`);
        console.log(`   ├─ Confidence: ${(confidence*100).toFixed(1)}%`);
        console.log(`   └─ Risk Level: ${riskLevel}`);

        res.json({
            success: true,
            message: "Prediksi ML berhasil disimpan",
            data: data
        });
    } catch (err) {
        console.error(" Error update prediksi:", err);
        res.status(500).json({ error: "Gagal menyimpan prediksi" });
    }
});

// GET semua data dengan pagination
app.get('/api/data/all', async (req, res) => {
    try {
        const page = parseInt(req.query.page) || 1;
        const limit = parseInt(req.query.limit) || 50;
        const skip = (page - 1) * limit;
        
        const total = await SensorData.countDocuments();
        const data = await SensorData.find()
            .sort({ timestamp: -1 })
            .skip(skip)
            .limit(limit);
        
        res.json({
            success: true,
            count: data.length,
            total: total,
            page: page,
            totalPages: Math.ceil(total / limit),
            data: data
        });
    } catch (err) {
        console.error(" Gagal mengambil data:", err);
        res.status(500).json({ error: "Gagal mengambil data" });
    }
});

// GET data dengan ML prediction (untuk history)
app.get('/api/data/history', async (req, res) => {
    try {
        const page = parseInt(req.query.page) || 1;
        const limit = parseInt(req.query.limit) || 20;
        const skip = (page - 1) * limit;
        
        const total = await SensorData.countDocuments({ 
            'mlPrediction.disease': { $ne: null } 
        });
        
        const data = await SensorData.find({ 
            'mlPrediction.disease': { $ne: null } 
        })
            .sort({ timestamp: -1 })
            .skip(skip)
            .limit(limit);
        
        res.json({
            success: true,
            count: data.length,
            total: total,
            page: page,
            totalPages: Math.ceil(total / limit),
            data: data
        });
    } catch (err) {
        console.error(" Gagal mengambil history:", err);
        res.status(500).json({ error: "Gagal mengambil history" });
    }
});

// GET statistik dengan ML
app.get('/api/stats', async (req, res) => {
    try {
        const totalData = await SensorData.countDocuments();
        const totalWithML = await SensorData.countDocuments({ 
            'mlPrediction.disease': { $ne: null } 
        });
        
        const avgStats = await SensorData.aggregate([
            {
                $group: {
                    _id: null,
                    avgPH: { $avg: "$ph" },
                    avgTDS: { $avg: "$tds" },
                    avgSG: { $avg: "$specificGravity" },
                    avgNTU: { $avg: "$turbidityNTU" },
                    avgConfidence: { $avg: "$mlPrediction.confidence" }
                }
            }
        ]);

        const diseaseDist = await SensorData.aggregate([
            { $match: { 'mlPrediction.disease': { $ne: null } } },
            {
                $group: {
                    _id: "$mlPrediction.disease",
                    count: { $sum: 1 },
                    avgConfidence: { $avg: "$mlPrediction.confidence" }
                }
            },
            { $sort: { count: -1 } }
        ]);

        res.json({
            success: true,
            totalData: totalData,
            totalWithML: totalWithML,
            mlCoverage: totalData > 0 ? ((totalWithML / totalData) * 100).toFixed(1) + '%' : '0%',
            averages: avgStats[0] || {},
            diseaseDistribution: diseaseDist
        });
    } catch (err) {
        console.error(" Gagal mengambil statistik:", err);
        res.status(500).json({ error: "Gagal mengambil statistik" });
    }
});

// Root endpoint
app.get('/', (req, res) => {
    res.json({
        message: "IoT Urine Analyzer API with ML Integration",
        version: "3.0 (Auto ML Prediction)",
        endpoints: {
            POST: {
                "/api/data": "Simpan data sensor (auto ML prediction)",
                "/api/prediction/:id": "Update prediksi ML manual"
            },
            GET: {
                "/api/data/all": "Semua data (pagination)",
                "/api/data/history": "Data dengan ML prediction (pagination)",
                "/api/stats": "Statistik lengkap"
            }
        }
    });
});

const PORT = 3000;
app.listen(PORT, () => {
    console.log(`
|===========================================|
|   IoT Urine Analyzer Server v3.0          |
|===========================================|
|   Server: http://localhost:${PORT}        |
|   Database: iot_urine                     |
|   ML Integration:  ACTIVE                 |
|===========================================|
    `);
});

process.on('SIGINT', async () => {
    console.log('\n Mematikan server...');
    await mongoose.connection.close();
    console.log(' MongoDB disconnected');
    process.exit(0);
});